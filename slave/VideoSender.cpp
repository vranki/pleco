#include "VideoSender.h"

#include <QObject>
#include <QDebug>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <glib.h>

// High quality: 1024kbps, low quality: 256kbps
static const int video_quality_bitrate[] = {256, 1024, 2048};

VideoSender::VideoSender(Hardware *hardware):
  QObject(), pipeline(NULL), videoSource("v4l2src"), hardware(hardware),
  encoder(NULL), bitrate(video_quality_bitrate[0]), quality(0)
{

#ifndef GLIB_VERSION_2_32
  // Must initialise GLib and its threading system
  g_type_init();
  if (!g_thread_supported()) {
	g_thread_init(NULL);
  }
#endif

}



VideoSender::~VideoSender()
{ 

  // Clean up
  qDebug() << "Stopping video encoding";
  if (pipeline) {
	gst_element_set_state(pipeline, GST_STATE_NULL);
  }

  qDebug() << "Deleting pipeline";
  if (pipeline) {
	gst_object_unref(GST_OBJECT(pipeline));
	pipeline = NULL;
  }

}



bool VideoSender::enableSending(bool enable)
{
  GstElement *sink;
  GError *error = NULL;

  qDebug() << "In" << __FUNCTION__ << ", Enable:" << enable;

  // Disable video sending
  if (enable == false) {
	qDebug() << "Stopping video encoding";
	if (pipeline) {
	  gst_element_set_state(pipeline, GST_STATE_NULL);
	}

	qDebug() << "Deleting pipeline";
	if (pipeline) {
	  gst_object_unref(GST_OBJECT(pipeline));
	  pipeline = NULL;
	}
	encoder = NULL;
	return true;
  }

  if (pipeline) {
	// Do nothing as the pipeline has already been created and is
	// probably running
	qCritical("Pipeline exists already, doing nothing");
	return true;
  }

  // Initialisation. We don't pass command line arguments here
  if (!gst_init_check(NULL, NULL, NULL)) {
	qCritical("Failed to init GST");
	return false;
  }

  if (!hardware) {
	qCritical("No hardware plugin");
	return false;
  }

  QString pipelineString = "";
  pipelineString.append(videoSource + " name=source");
  pipelineString.append(" ! ");
  switch(quality) {
  default:
  case 0:
	pipelineString.append("capsfilter caps=\"video/x-raw-yuv,width=(int)320,height=(int)240,framerate=(fraction)30/1\"");
	break;
  case 1:
	pipelineString.append("capsfilter caps=\"video/x-raw-yuv,width=(int)640,height=(int)480,framerate=(fraction)30/1\"");
	break;
  case 2:
	pipelineString.append("capsfilter caps=\"video/x-raw-yuv,width=(int)800,height=(int)600,framerate=(fraction)30/1\"");
	break;
  }
  pipelineString.append(" ! ");
  pipelineString.append(hardware->getEncodingPipeline());
  pipelineString.append(" ! ");
  pipelineString.append("rtph264pay name=rtppay config-interval=1");
  pipelineString.append(" ! ");
  pipelineString.append("appsink name=sink sync=false max-buffers=1 drop=true");

  qDebug() << "Using pipeline:" << pipelineString;

  // Create encoding video pipeline
  pipeline = gst_parse_launch(pipelineString.toUtf8(), &error);
  if (!pipeline) {
	qCritical("Failed to parse pipeline: %s", error->message);
	g_error_free(error);
    return false;
  }

  encoder = gst_bin_get_by_name(GST_BIN(pipeline), "encoder");
  if (!encoder) {
	qCritical("Failed to get encoder");
    return false;
  }

  // Assuming here that X86 uses x264enc
  if (hardware->getHardwareName() == "generic_x86") {
	//g_object_set(G_OBJECT(encoder), "speed-preset", 1, NULL);
	g_object_set(G_OBJECT(encoder), "tune", 0x00000004, NULL);
	g_object_set(G_OBJECT(encoder), "profile", 3, NULL);
  }

  if (hardware->getHardwareName() == "tegrak1") {
	g_object_set(G_OBJECT(encoder), "input-buffers", 2, NULL);
	g_object_set(G_OBJECT(encoder), "output-buffers", 2, NULL);
	//g_object_set(G_OBJECT(encoder), "quality-level", 0, NULL);
	//g_object_set(G_OBJECT(encoder), "rc-mode", 0, NULL);
  }

  setBitrate(bitrate);

  {
	GstElement *source;
	source = gst_bin_get_by_name(GST_BIN(pipeline), "source");
	if (!source) {
	  qCritical("Failed to get source");
	  return false;
	}

	g_object_set(G_OBJECT(source), "do-timestamp", true, NULL);

	if (videoSource == "videotestsrc") {
	  g_object_set(G_OBJECT(source), "is-live", true, NULL);
	} else if (videoSource == "v4l2src") {
	  //g_object_set(G_OBJECT(source), "always-copy", false, NULL);
	}
  }


  sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
  if (!sink) {
	qCritical("Failed to get sink");
    return false;
  }

  g_object_set(G_OBJECT(sink), "sync", false, NULL);

  gst_app_sink_set_max_buffers(GST_APP_SINK(sink), 2);// 8 buffers is hopefully enough
  gst_app_sink_set_drop(GST_APP_SINK(sink), true); // drop old data, if needed

  // Set appsink callbacks
  GstAppSinkCallbacks appSinkCallbacks;
  appSinkCallbacks.eos             = NULL;
  appSinkCallbacks.new_preroll     = NULL;
  appSinkCallbacks.new_buffer      = &newBufferCB;
  appSinkCallbacks.new_buffer_list = NULL;

  gst_app_sink_set_callbacks(GST_APP_SINK(sink), &appSinkCallbacks, this, NULL);

  // Start running 
  gst_element_set_state(GST_ELEMENT(pipeline), GST_STATE_PLAYING);

  return true;
}



void VideoSender::emitMedia(QByteArray *data)
{
  qDebug() << "In" << __FUNCTION__;

  emit(media(data));

}



GstFlowReturn VideoSender::newBufferCB(GstAppSink *sink, gpointer user_data)
{
  qDebug() << "In" << __FUNCTION__;

  VideoSender *vs = static_cast<VideoSender *>(user_data);

  // Get new video buffer
  GstBuffer *buffer = gst_app_sink_pull_buffer(GST_APP_SINK(sink));

  if (buffer == NULL) {
	qWarning("%s: Failed to get new buffer", __FUNCTION__);
	return GST_FLOW_OK;
  }
  
  // Copy the data to QBytArray
  // FIXME: zero copy?
  QByteArray *data = new QByteArray((char *)(buffer->data), (int)(buffer->size));
  gst_buffer_unref(buffer);

  vs->emitMedia(data);

  return GST_FLOW_OK;
}



void VideoSender::setVideoSource(int index)
{
  switch (index) {
  case 0: // v4l2src
	videoSource = "v4l2src";
	break;
  case 1: // videotestsrc
	videoSource = "videotestsrc";
	break;
  default:
	qWarning("%s: Unknown video source index: %d", __FUNCTION__, index);
  }

}



void VideoSender::setBitrate(int bitrate)
{

  qDebug() << "In" << __FUNCTION__ << ", bitrate:" << bitrate;

  if (!encoder) {
    if (pipeline) {
      qWarning("Pipeline found, but no encoder?");
    }
    return;
  }

  int tmpbitrate = bitrate;
  if (!hardware->bitrateInKilobits()) {
    tmpbitrate *= 1024;
  }

  qDebug() << "In" << __FUNCTION__ << ", setting bitrate:" << tmpbitrate;
  g_object_set(G_OBJECT(encoder), "bitrate", tmpbitrate, NULL);
}



void VideoSender::setVideoQuality(quint16 q)
{
  quality = q;

  if (quality < sizeof(video_quality_bitrate)) {
	bitrate = video_quality_bitrate[quality];
  } else {
	qWarning("%s: Unknown quality: %d", __FUNCTION__, quality);
	bitrate = video_quality_bitrate[0];
  }

  setBitrate(bitrate);
}
