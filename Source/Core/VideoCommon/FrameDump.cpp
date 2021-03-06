// Copyright 2009 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "VideoCommon/FrameDump.h"

#if defined(__FreeBSD__)
#define __STDC_CONSTANT_MACROS 1
#endif

#include <sstream>
#include <string>

#include <fmt/chrono.h>
#include <fmt/format.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
#include <libswscale/swscale.h>
}

#include "Common/ChunkFile.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"

#include "Core/ConfigManager.h"
#include "Core/HW/SystemTimers.h"
#include "Core/HW/VideoInterface.h"

#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/VideoConfig.h"

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(55, 28, 1)
#define AV_CODEC_FLAG_GLOBAL_HEADER CODEC_FLAG_GLOBAL_HEADER
#define av_frame_alloc avcodec_alloc_frame
#define av_frame_free avcodec_free_frame
#endif

struct FrameDumpContext
{
  AVFormatContext* format = nullptr;
  AVStream* stream = nullptr;
  AVCodecContext* codec = nullptr;
  AVFrame* src_frame = nullptr;
  AVFrame* scaled_frame = nullptr;
  SwsContext* sws = nullptr;

  s64 last_pts = AV_NOPTS_VALUE;

  int width = 0;
  int height = 0;

  u64 first_frame_ticks = 0;
  u32 savestate_index = 0;

  bool gave_vfr_warning = false;
};

namespace
{
AVRational GetTimeBaseForCurrentRefreshRate()
{
  int num;
  int den;
  av_reduce(&num, &den, int(VideoInterface::GetTargetRefreshRateDenominator()),
            int(VideoInterface::GetTargetRefreshRateNumerator()), std::numeric_limits<int>::max());
  return AVRational{num, den};
}

void InitAVCodec()
{
  static bool first_run = true;
  if (first_run)
  {
#if LIBAVCODEC_VERSION_MICRO >= 100 && LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 9, 100)
    av_register_all();
#endif
    // TODO: We never call avformat_network_deinit.
    avformat_network_init();
    first_run = false;
  }
}

bool AVStreamCopyContext(AVStream* stream, AVCodecContext* codec_context)
{
#if (LIBAVCODEC_VERSION_MICRO >= 100 && LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(57, 33, 100)) ||  \
    (LIBAVCODEC_VERSION_MICRO < 100 && LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(57, 5, 0))

  stream->time_base = codec_context->time_base;
  return avcodec_parameters_from_context(stream->codecpar, codec_context) >= 0;
#else
  return avcodec_copy_context(stream->codec, codec_context) >= 0;
#endif
}

std::string GetDumpPath(const std::string& extension, std::time_t time, u32 index)
{
  if (!g_Config.sDumpPath.empty())
    return g_Config.sDumpPath;

  const std::string path_prefix =
      File::GetUserPath(D_DUMPFRAMES_IDX) + SConfig::GetInstance().GetGameID();

  const std::string base_name =
      fmt::format("{}_{:%Y-%m-%d_%H-%M-%S}_{}", path_prefix, *std::localtime(&time), index);

  const std::string path = fmt::format("{}.{}", base_name, extension);

  // Ask to delete file.
  if (File::Exists(path))
  {
    if (SConfig::GetInstance().m_DumpFramesSilent ||
        AskYesNoT("Delete the existing file '%s'?", path.c_str()))
    {
      File::Delete(path);
    }
    else
    {
      // Stop and cancel dumping the video
      return "";
    }
  }

  return path;
}

int ReceivePacket(AVCodecContext* avctx, AVPacket* pkt, int* got_packet)
{
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(57, 37, 100)
  return avcodec_encode_video2(avctx, pkt, nullptr, got_packet);
#else
  *got_packet = 0;

  const int error = avcodec_receive_packet(avctx, pkt);
  if (!error)
    *got_packet = 1;

  if (error == AVERROR(EAGAIN) || error == AVERROR_EOF)
    return 0;

  return error;
#endif
}

int SendFrameAndReceivePacket(AVCodecContext* avctx, AVPacket* pkt, AVFrame* frame, int* got_packet)
{
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(57, 37, 100)
  return avcodec_encode_video2(avctx, pkt, frame, got_packet);
#else
  *got_packet = 0;

  const int error = avcodec_send_frame(avctx, frame);
  if (error)
    return error;

  return ReceivePacket(avctx, pkt, got_packet);
#endif
}

void WritePacket(AVPacket& pkt, const FrameDumpContext& context)
{
  av_packet_rescale_ts(&pkt, context.codec->time_base, context.stream->time_base);

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(56, 60, 100)
  if (context.codec->coded_frame->key_frame)
    pkt.flags |= AV_PKT_FLAG_KEY;
#endif

  pkt.stream_index = context.stream->index;
  av_interleaved_write_frame(context.format, &pkt);
}

}  // namespace

bool FrameDump::Start(int w, int h)
{
  if (IsStarted())
    return true;

  m_savestate_index = 0;
  m_start_time = std::time(nullptr);
  m_file_index = 0;

  return PrepareEncoding(w, h);
}

bool FrameDump::PrepareEncoding(int w, int h)
{
  m_context = std::make_unique<FrameDumpContext>();

  m_context->width = w;
  m_context->height = h;

  InitAVCodec();
  const bool success = CreateVideoFile();
  if (!success)
  {
    CloseVideoFile();
    OSD::AddMessage("FrameDump Start failed");
  }
  return success;
}

bool FrameDump::CreateVideoFile()
{
  const std::string& format = g_Config.sDumpFormat;

  const std::string dump_path = GetDumpPath(format, m_start_time, m_file_index);

  if (dump_path.empty())
    return false;

  File::CreateFullPath(dump_path);

  AVOutputFormat* const output_format = av_guess_format(format.c_str(), dump_path.c_str(), nullptr);
  if (!output_format)
  {
    ERROR_LOG(VIDEO, "Invalid format %s", format.c_str());
    return false;
  }

  if (avformat_alloc_output_context2(&m_context->format, output_format, nullptr,
                                     dump_path.c_str()) < 0)
  {
    ERROR_LOG(VIDEO, "Could not allocate output context");
    return false;
  }

  const std::string& codec_name = g_Config.bUseFFV1 ? "ffv1" : g_Config.sDumpCodec;

  AVCodecID codec_id = output_format->video_codec;

  if (!codec_name.empty())
  {
    const AVCodecDescriptor* const codec_desc = avcodec_descriptor_get_by_name(codec_name.c_str());
    if (codec_desc)
      codec_id = codec_desc->id;
    else
      WARN_LOG(VIDEO, "Invalid codec %s", codec_name.c_str());
  }

  const AVCodec* codec = nullptr;

  if (!g_Config.sDumpEncoder.empty())
  {
    codec = avcodec_find_encoder_by_name(g_Config.sDumpEncoder.c_str());
    if (!codec)
      WARN_LOG(VIDEO, "Invalid encoder %s", g_Config.sDumpEncoder.c_str());
  }
  if (!codec)
    codec = avcodec_find_encoder(codec_id);

  m_context->codec = avcodec_alloc_context3(codec);
  if (!codec || !m_context->codec)
  {
    ERROR_LOG(VIDEO, "Could not find encoder or allocate codec context");
    return false;
  }

  // Force XVID FourCC for better compatibility when using H.263
  if (codec->id == AV_CODEC_ID_MPEG4)
    m_context->codec->codec_tag = MKTAG('X', 'V', 'I', 'D');

  const auto time_base = GetTimeBaseForCurrentRefreshRate();

  INFO_LOG_FMT(VIDEO, "Creating video file: {} x {} @ {}/{} fps", m_context->width,
               m_context->height, time_base.den, time_base.num);

  m_context->codec->codec_type = AVMEDIA_TYPE_VIDEO;
  m_context->codec->bit_rate = static_cast<int64_t>(g_Config.iBitrateKbps) * 1000;
  m_context->codec->width = m_context->width;
  m_context->codec->height = m_context->height;
  m_context->codec->time_base = time_base;
  m_context->codec->gop_size = 1;
  m_context->codec->level = 1;
  m_context->codec->pix_fmt = g_Config.bUseFFV1 ? AV_PIX_FMT_BGR0 : AV_PIX_FMT_YUV420P;

  if (output_format->flags & AVFMT_GLOBALHEADER)
    m_context->codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  if (avcodec_open2(m_context->codec, codec, nullptr) < 0)
  {
    ERROR_LOG(VIDEO, "Could not open codec");
    return false;
  }

  m_context->src_frame = av_frame_alloc();
  m_context->scaled_frame = av_frame_alloc();

  m_context->scaled_frame->format = m_context->codec->pix_fmt;
  m_context->scaled_frame->width = m_context->width;
  m_context->scaled_frame->height = m_context->height;

#if LIBAVCODEC_VERSION_MAJOR >= 55
  if (av_frame_get_buffer(m_context->scaled_frame, 1))
    return false;
#else
  if (avcodec_default_get_buffer(m_context->codec, m_context->scaled_frame))
    return false;
#endif

  m_context->stream = avformat_new_stream(m_context->format, codec);
  if (!m_context->stream || !AVStreamCopyContext(m_context->stream, m_context->codec))
  {
    ERROR_LOG(VIDEO, "Could not create stream");
    return false;
  }

  NOTICE_LOG(VIDEO, "Opening file %s for dumping", dump_path.c_str());
  if (avio_open(&m_context->format->pb, dump_path.c_str(), AVIO_FLAG_WRITE) < 0 ||
      avformat_write_header(m_context->format, nullptr))
  {
    ERROR_LOG(VIDEO, "Could not open %s", dump_path.c_str());
    return false;
  }

  if (av_cmp_q(m_context->stream->time_base, time_base) != 0)
  {
    WARN_LOG_FMT(VIDEO, "Stream time base differs at {}/{}", m_context->stream->time_base.den,
                 m_context->stream->time_base.num);
  }

  OSD::AddMessage(fmt::format("Dumping Frames to \"{}\" ({}x{})", dump_path, m_context->width,
                              m_context->height));
  return true;
}

bool FrameDump::IsFirstFrameInCurrentFile() const
{
  return m_context->last_pts == AV_NOPTS_VALUE;
}

void FrameDump::AddFrame(const FrameData& frame)
{
  // Are we even dumping?
  if (!IsStarted())
    return;

  CheckForConfigChange(frame);

  // Handle failure after a config change.
  if (!IsStarted())
    return;

  if (IsFirstFrameInCurrentFile())
  {
    m_context->first_frame_ticks = frame.state.ticks;
    m_context->savestate_index = frame.state.savestate_index;
  }

  // Calculate presentation timestamp from current ticks since first frame ticks.
  const s64 pts = av_rescale_q(frame.state.ticks - m_context->first_frame_ticks,
                               AVRational{1, int(SystemTimers::GetTicksPerSecond())},
                               m_context->codec->time_base);

  if (!IsFirstFrameInCurrentFile())
  {
    if (pts <= m_context->last_pts)
    {
      WARN_LOG(VIDEO, "PTS delta < 1. Current frame will not be dumped.");
      return;
    }
    else if (pts > m_context->last_pts + 1 && !m_context->gave_vfr_warning)
    {
      WARN_LOG(VIDEO, "PTS delta > 1. Resulting file will have variable frame rate. "
                      "Subsequent occurances will not be reported.");
      m_context->gave_vfr_warning = true;
    }
  }

  constexpr AVPixelFormat pix_fmt = AV_PIX_FMT_RGBA;

  m_context->src_frame->data[0] = const_cast<u8*>(frame.data);
  m_context->src_frame->linesize[0] = frame.stride;
  m_context->src_frame->format = pix_fmt;
  m_context->src_frame->width = m_context->width;
  m_context->src_frame->height = m_context->height;

  // Convert image from RGBA to desired pixel format.
  m_context->sws = sws_getCachedContext(
      m_context->sws, frame.width, frame.height, pix_fmt, m_context->width, m_context->height,
      m_context->codec->pix_fmt, SWS_BICUBIC, nullptr, nullptr, nullptr);
  if (m_context->sws)
  {
    sws_scale(m_context->sws, m_context->src_frame->data, m_context->src_frame->linesize, 0,
              frame.height, m_context->scaled_frame->data, m_context->scaled_frame->linesize);
  }

  m_context->last_pts = pts;
  m_context->scaled_frame->pts = pts;

  // Encode and write the image.
  AVPacket pkt;
  av_init_packet(&pkt);

  int got_packet = 0;
  const int error =
      SendFrameAndReceivePacket(m_context->codec, &pkt, m_context->scaled_frame, &got_packet);

  if (error)
  {
    ERROR_LOG(VIDEO, "Error while encoding video: %d", error);
    return;
  }

  if (got_packet)
    WritePacket(pkt, *m_context);

  HandleDelayedPackets();
}

void FrameDump::HandleDelayedPackets()
{
  while (true)
  {
    AVPacket pkt;
    av_init_packet(&pkt);

    int got_packet = 0;
    const int error = ReceivePacket(m_context->codec, &pkt, &got_packet);
    if (error)
    {
      ERROR_LOG(VIDEO, "Error while encoding delayed frames: %d", error);
      break;
    }

    if (!got_packet)
      break;

    WritePacket(pkt, *m_context);
  }
}

void FrameDump::Stop()
{
  if (!IsStarted())
    return;

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(57, 37, 100)
  // Signal end of stream to encoder.
  if (const int flush_error = avcodec_send_frame(m_context->codec, nullptr))
    WARN_LOG_FMT(VIDEO, "Error sending flush packet: {}", flush_error);
#endif

  HandleDelayedPackets();
  av_write_trailer(m_context->format);
  CloseVideoFile();
  NOTICE_LOG(VIDEO, "Stopping frame dump");
  OSD::AddMessage("Stopped dumping frames");
}

bool FrameDump::IsStarted() const
{
  return m_context != nullptr;
}

void FrameDump::CloseVideoFile()
{
  av_frame_free(&m_context->src_frame);
  av_frame_free(&m_context->scaled_frame);

  avcodec_free_context(&m_context->codec);

  if (m_context->format)
    avio_closep(&m_context->format->pb);

  avformat_free_context(m_context->format);

  if (m_context->sws)
    sws_freeContext(m_context->sws);

  m_context.reset();
}

void FrameDump::DoState(PointerWrap& p)
{
  if (p.GetMode() == PointerWrap::MODE_READ)
    ++m_savestate_index;
}

void FrameDump::CheckForConfigChange(const FrameData& frame)
{
  bool restart_dump = false;

  // We check here to see if the requested width and height have changed since the last frame which
  // was dumped, then create a new file accordingly. However, is it possible for the height
  // (possibly width as well, but no examples known) to have a value of zero. This can occur as the
  // VI is able to be set to a zero value for height/width to disable output. If this is the case,
  // simply keep the last known resolution of the video for the added frame.
  if ((frame.width != m_context->width || frame.height != m_context->height) &&
      (frame.width > 0 && frame.height > 0))
  {
    INFO_LOG(VIDEO, "Starting new dump on resolution change.");
    restart_dump = true;
  }
  else if (!IsFirstFrameInCurrentFile() &&
           frame.state.savestate_index != m_context->savestate_index)
  {
    INFO_LOG(VIDEO, "Starting new dump on savestate load.");
    restart_dump = true;
  }
  else if (frame.state.refresh_rate_den != m_context->codec->time_base.num ||
           frame.state.refresh_rate_num != m_context->codec->time_base.den)
  {
    INFO_LOG_FMT(VIDEO, "Starting new dump on refresh rate change {}/{} vs {}/{}.",
                 m_context->codec->time_base.den, m_context->codec->time_base.num,
                 frame.state.refresh_rate_num, frame.state.refresh_rate_den);
    restart_dump = true;
  }

  if (restart_dump)
  {
    Stop();
    ++m_file_index;
    PrepareEncoding(frame.width, frame.height);
  }
}

FrameDump::FrameState FrameDump::FetchState(u64 ticks) const
{
  FrameState state;
  state.ticks = ticks;
  state.savestate_index = m_savestate_index;

  const auto time_base = GetTimeBaseForCurrentRefreshRate();
  state.refresh_rate_num = time_base.den;
  state.refresh_rate_den = time_base.num;
  return state;
}

FrameDump::FrameDump() = default;

FrameDump::~FrameDump()
{
  Stop();
}
