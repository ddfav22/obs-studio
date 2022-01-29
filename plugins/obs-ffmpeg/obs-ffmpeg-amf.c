/******************************************************************************
    Copyright (C) 2022 by Hugh Bailey <obs.jim@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include <util/darray.h>
#include <util/dstr.h>
#include <util/base.h>
#include <media-io/video-io.h>
#include <obs-module.h>

#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

#include "obs-ffmpeg-formats.h"

#define do_log(level, format, ...)                \
	blog(level, "[ffmpeg-amf: '%s'] " format, \
	     obs_encoder_get_name(enc->encoder), ##__VA_ARGS__)

#define warn(format, ...) do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...) do_log(LOG_INFO, format, ##__VA_ARGS__)
#define debug(format, ...) do_log(LOG_DEBUG, format, ##__VA_ARGS__)

struct ffmpeg_amf_encoder {
	obs_encoder_t *encoder;

	AVCodec *ffmpeg_amf;
	AVCodecContext *context;

	AVFrame *vframe;

	DARRAY(uint8_t) buffer;
	DARRAY(uint8_t) header;

	int height;
	bool first_packet;
	bool initialized;
};

static const char *ffmpeg_amf_avc_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "FFmpeg AMF H.264";
}

static const char *ffmpeg_amf_hevc_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "FFmpeg AMF H.265";
}

static inline bool valid_format(enum video_format format)
{
	return format == VIDEO_FORMAT_I420 || format == VIDEO_FORMAT_NV12;
}

static void ffmpeg_amf_video_info(void *data, struct video_scale_info *info)
{
	struct ffmpeg_amf_encoder *enc = data;
	enum video_format pref_format;

	pref_format = obs_encoder_get_preferred_video_format(enc->encoder);

	if (!valid_format(pref_format)) {
		pref_format = valid_format(info->format) ? info->format
							 : VIDEO_FORMAT_NV12;
	}

	info->format = pref_format;
}

static bool ffmpeg_amf_init_codec(struct ffmpeg_amf_encoder *enc)
{
	int ret;

	enc->context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

	ret = avcodec_open2(enc->context, enc->ffmpeg_amf, NULL);
	if (ret < 0) {
		if (!obs_encoder_get_last_error(enc->encoder)) {
			struct dstr error_message = {0};

			dstr_copy(&error_message,
				  obs_module_text("NVENC.Error"));
			dstr_replace(&error_message, "%1", av_err2str(ret));
			dstr_cat(&error_message, "\r\n\r\n");
			dstr_cat(&error_message,
				 obs_module_text("NVENC.CheckDrivers"));

			obs_encoder_set_last_error(enc->encoder,
						   error_message.array);
			dstr_free(&error_message);
		}
		warn("Failed to open NVENC codec: %s", av_err2str(ret));
		return false;
	}

	enc->vframe = av_frame_alloc();
	if (!enc->vframe) {
		warn("Failed to allocate video frame");
		return false;
	}

	enc->vframe->format = enc->context->pix_fmt;
	enc->vframe->width = enc->context->width;
	enc->vframe->height = enc->context->height;
	enc->vframe->colorspace = enc->context->colorspace;
	enc->vframe->color_range = enc->context->color_range;

	ret = av_frame_get_buffer(enc->vframe, base_get_alignment());
	if (ret < 0) {
		warn("Failed to allocate vframe: %s", av_err2str(ret));
		return false;
	}

	enc->initialized = true;
	return true;
}

enum RC_MODE { RC_MODE_CBR, RC_MODE_VBR, RC_MODE_CQP, RC_MODE_LOSSLESS };

static bool ffmpeg_amf_update(struct ffmpeg_amf_encoder *enc,
			      obs_data_t *settings)
{
	const char *rc = obs_data_get_string(settings, "rate_control");
	int bitrate = (int)obs_data_get_int(settings, "bitrate");
	int cqp = (int)obs_data_get_int(settings, "cqp");
	int keyint_sec = (int)obs_data_get_int(settings, "keyint_sec");
	const char *preset = obs_data_get_string(settings, "preset");
	const char *profile = obs_data_get_string(settings, "profile");

	video_t *video = obs_encoder_video(enc->encoder);
	const struct video_output_info *voi = video_output_get_info(video);
	struct video_scale_info info;

	info.format = voi->format;
	info.colorspace = voi->colorspace;
	info.range = voi->range;

	bool twopass = false;

	ffmpeg_amf_video_info(enc, &info);
	av_opt_set(enc->context->priv_data, "profile", profile, 0);
	av_opt_set(enc->context->priv_data, "preset", preset, 0);

	if (astrcmpi(rc, "cqp") == 0) {
		av_opt_set(enc->context->priv_data, "rc", "cqp", 0);
		bitrate = 0;
		enc->context->global_quality = cqp;

	} else {
		const int64_t rate = bitrate * 1000LL;

		if (astrcmpi(rc, "vbr") == 0) {
			av_opt_set(enc->context->priv_data, "rc", "vbr_peak",
				   0);
		} else { /* CBR by default */
			av_opt_set(enc->context->priv_data, "rc", "cbr", 0);
			enc->context->rc_min_rate = rate;
		}

		enc->context->rc_max_rate = rate;
		cqp = 0;
	}

	av_opt_set(enc->context->priv_data, "level", "auto", 0);
	av_opt_set_int(enc->context->priv_data, "2pass", twopass, 0);

	const int rate = bitrate * 1000;
	enc->context->bit_rate = rate;
	enc->context->rc_buffer_size = rate;
	enc->context->width = obs_encoder_get_width(enc->encoder);
	enc->context->height = obs_encoder_get_height(enc->encoder);
	enc->context->time_base = (AVRational){voi->fps_den, voi->fps_num};
	enc->context->pix_fmt = obs_to_ffmpeg_video_format(info.format);
	enc->context->color_range = info.range == VIDEO_RANGE_FULL
					    ? AVCOL_RANGE_JPEG
					    : AVCOL_RANGE_MPEG;

	switch (info.colorspace) {
	case VIDEO_CS_601:
		enc->context->color_trc = AVCOL_TRC_SMPTE170M;
		enc->context->color_primaries = AVCOL_PRI_SMPTE170M;
		enc->context->colorspace = AVCOL_SPC_SMPTE170M;
		break;
	case VIDEO_CS_DEFAULT:
	case VIDEO_CS_709:
		enc->context->color_trc = AVCOL_TRC_BT709;
		enc->context->color_primaries = AVCOL_PRI_BT709;
		enc->context->colorspace = AVCOL_SPC_BT709;
		break;
	case VIDEO_CS_SRGB:
		enc->context->color_trc = AVCOL_TRC_IEC61966_2_1;
		enc->context->color_primaries = AVCOL_PRI_BT709;
		enc->context->colorspace = AVCOL_SPC_BT709;
		break;
	}

	if (keyint_sec)
		enc->context->gop_size =
			keyint_sec * voi->fps_num / voi->fps_den;
	else
		enc->context->gop_size = 250;

	enc->height = enc->context->height;

	info("settings:\n"
	     "\trate_control: %s\n"
	     "\tbitrate:      %d\n"
	     "\tcqp:          %d\n"
	     "\tkeyint:       %d\n"
	     "\tpreset:       %s\n"
	     "\tprofile:      %s\n"
	     "\twidth:        %d\n"
	     "\theight:       %d\n",
	     rc, bitrate, cqp, enc->context->gop_size, preset, profile,
	     enc->context->width, enc->context->height);

	return ffmpeg_amf_init_codec(enc);
}

static bool ffmpeg_amf_reconfigure(void *data, obs_data_t *settings)
{
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(58, 19, 101)
	struct ffmpeg_amf_encoder *enc = data;

	const int64_t bitrate = obs_data_get_int(settings, "bitrate");
	const char *rc = obs_data_get_string(settings, "rate_control");
	bool cbr = astrcmpi(rc, "CBR") == 0;
	bool vbr = astrcmpi(rc, "VBR") == 0;
	if (cbr || vbr) {
		const int64_t rate = bitrate * 1000;
		enc->context->bit_rate = rate;
		enc->context->rc_max_rate = rate;
	}
#endif
	return true;
}

static void ffmpeg_amf_destroy(void *data)
{
	struct ffmpeg_amf_encoder *enc = data;

	if (enc->initialized) {
		AVPacket pkt = {0};
		int r_pkt = 1;

		while (r_pkt) {
#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(57, 40, 101)
			if (avcodec_receive_packet(enc->context, &pkt) < 0)
				break;
#else
			if (avcodec_encode_video2(enc->context, &pkt, NULL,
						  &r_pkt) < 0)
				break;
#endif

			if (r_pkt)
				av_packet_unref(&pkt);
		}
	}

	avcodec_close(enc->context);
	av_frame_unref(enc->vframe);
	av_frame_free(&enc->vframe);
	da_free(enc->buffer);
	da_free(enc->header);

	bfree(enc);
}

static void *ffmpeg_amf_create(obs_data_t *settings, obs_encoder_t *encoder,
			       bool hevc)
{
	struct ffmpeg_amf_encoder *enc;

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 9, 100)
	avcodec_register_all();
#endif

	enc = bzalloc(sizeof(*enc));
	enc->encoder = encoder;
	enc->ffmpeg_amf =
		avcodec_find_encoder_by_name(hevc ? "hevc_amf" : "h264_amf");
	enc->first_packet = true;

	blog(LOG_INFO, "---------------------------------");

	if (!enc->ffmpeg_amf) {
		obs_encoder_set_last_error(encoder,
					   "Couldn't find AMF encoder");
		warn("Couldn't find encoder");
		goto fail;
	}

	enc->context = avcodec_alloc_context3(enc->ffmpeg_amf);
	if (!enc->context) {
		warn("Failed to create codec context");
		goto fail;
	}

	if (!ffmpeg_amf_update(enc, settings))
		goto fail;

	return enc;

fail:
	ffmpeg_amf_destroy(enc);
	return NULL;
}

static void *ffmpeg_amf_avc_create(obs_data_t *settings, obs_encoder_t *encoder)
{
	return ffmpeg_amf_create(settings, encoder, false);
}

static void *ffmpeg_amf_hevc_create(obs_data_t *settings,
				    obs_encoder_t *encoder)
{
	return ffmpeg_amf_create(settings, encoder, true);
}

static inline void copy_data(AVFrame *pic, const struct encoder_frame *frame,
			     int height, enum AVPixelFormat format)
{
	int h_chroma_shift, v_chroma_shift;
	av_pix_fmt_get_chroma_sub_sample(format, &h_chroma_shift,
					 &v_chroma_shift);
	for (int plane = 0; plane < MAX_AV_PLANES; plane++) {
		if (!frame->data[plane])
			continue;

		int frame_rowsize = (int)frame->linesize[plane];
		int pic_rowsize = pic->linesize[plane];
		int bytes = frame_rowsize < pic_rowsize ? frame_rowsize
							: pic_rowsize;
		int plane_height = height >> (plane ? v_chroma_shift : 0);

		for (int y = 0; y < plane_height; y++) {
			int pos_frame = y * frame_rowsize;
			int pos_pic = y * pic_rowsize;

			memcpy(pic->data[plane] + pos_pic,
			       frame->data[plane] + pos_frame, bytes);
		}
	}
}

static bool ffmpeg_amf_encode(void *data, struct encoder_frame *frame,
			      struct encoder_packet *packet,
			      bool *received_packet)
{
	struct ffmpeg_amf_encoder *enc = data;
	AVPacket av_pkt = {0};
	int got_packet;
	int ret;

	av_init_packet(&av_pkt);

	copy_data(enc->vframe, frame, enc->height, enc->context->pix_fmt);

	enc->vframe->pts = frame->pts;
#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(57, 40, 101)
	ret = avcodec_send_frame(enc->context, enc->vframe);
	if (ret == 0)
		ret = avcodec_receive_packet(enc->context, &av_pkt);

	got_packet = (ret == 0);

	if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN))
		ret = 0;
#else
	ret = avcodec_encode_video2(enc->context, &av_pkt, enc->vframe,
				    &got_packet);
#endif
	if (ret < 0) {
		warn("ffmpeg_amf_encode: Error encoding: %s", av_err2str(ret));
		return false;
	}

	if (got_packet && av_pkt.size) {
		if (enc->first_packet) {
			if (enc->context->extradata_size) {
				da_copy_array(enc->header,
					      enc->context->extradata,
					      enc->context->extradata_size);
			}
			enc->first_packet = false;
		}

		da_copy_array(enc->buffer, av_pkt.data, av_pkt.size);

		packet->pts = av_pkt.pts;
		packet->dts = av_pkt.dts;
		packet->data = enc->buffer.array;
		packet->size = enc->buffer.num;
		packet->type = OBS_ENCODER_VIDEO;
		packet->keyframe = !!(av_pkt.flags & AV_PKT_FLAG_KEY);
		*received_packet = true;
	} else {
		*received_packet = false;
	}

	av_packet_unref(&av_pkt);
	return true;
}

void amf_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "bitrate", 2500);
	obs_data_set_default_int(settings, "cqp", 20);
	obs_data_set_default_string(settings, "rate_control", "CBR");
	obs_data_set_default_string(settings, "preset", "hq");
	obs_data_set_default_string(settings, "profile", "high");
}

static bool rate_control_modified(obs_properties_t *ppts, obs_property_t *p,
				  obs_data_t *settings)
{
	const char *rc = obs_data_get_string(settings, "rate_control");
	bool cqp = astrcmpi(rc, "CQP") == 0;

	p = obs_properties_get(ppts, "bitrate");
	obs_property_set_visible(p, !cqp);
	p = obs_properties_get(ppts, "cqp");
	obs_property_set_visible(p, cqp);
	return true;
}

static obs_properties_t *amf_properties_internal(bool hevc)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	p = obs_properties_add_list(props, "rate_control",
				    obs_module_text("RateControl"),
				    OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "CBR", "CBR");
	obs_property_list_add_string(p, "CQP", "CQP");
	obs_property_list_add_string(p, "VBR", "VBR");

	obs_property_set_modified_callback(p, rate_control_modified);

	p = obs_properties_add_int(props, "bitrate", obs_module_text("Bitrate"),
				   50, 300000, 50);
	obs_property_int_set_suffix(p, " Kbps");

	obs_properties_add_int(props, "cqp", obs_module_text("NVENC.CQLevel"),
			       1, 30, 1);

	obs_properties_add_int(props, "keyint_sec",
			       obs_module_text("KeyframeIntervalSec"), 0, 10,
			       1);

	p = obs_properties_add_list(props, "preset", obs_module_text("Preset"),
				    OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);

#define add_preset(val)                                                       \
	obs_property_list_add_string(p, obs_module_text("NVENC.Preset." val), \
				     val)
	add_preset("quality");
	add_preset("balanced");
	add_preset("speed");
#undef add_preset

	if (!hevc) {
		p = obs_properties_add_list(props, "profile",
					    obs_module_text("Profile"),
					    OBS_COMBO_TYPE_LIST,
					    OBS_COMBO_FORMAT_STRING);

#define add_profile(val) obs_property_list_add_string(p, val, val)
		add_profile("high");
		add_profile("main");
		add_profile("baseline");
#undef add_profile
	}

	return props;
}

obs_properties_t *amf_avc_properties(void *unused)
{
	UNUSED_PARAMETER(unused);
	return amf_properties_internal(false);
}

obs_properties_t *amf_hevc_properties(void *unused)
{
	UNUSED_PARAMETER(unused);
	return amf_properties_internal(true);
}

static bool ffmpeg_amf_extra_data(void *data, uint8_t **extra_data, size_t *size)
{
	struct ffmpeg_amf_encoder *enc = data;

	*extra_data = enc->header.array;
	*size = enc->header.num;
	return true;
}

struct obs_encoder_info ffmpeg_amf_avc_encoder_info = {
	.id = "h264_ffmpeg_amf",
	.type = OBS_ENCODER_VIDEO,
	.codec = "h264",
	.get_name = ffmpeg_amf_avc_getname,
	.create = ffmpeg_amf_avc_create,
	.destroy = ffmpeg_amf_destroy,
	.encode = ffmpeg_amf_encode,
	.update = ffmpeg_amf_reconfigure,
	.get_defaults = amf_defaults,
	.get_properties = amf_avc_properties,
	.get_extra_data = ffmpeg_amf_extra_data,
	.get_video_info = ffmpeg_amf_video_info,
#ifdef _WIN32
	.caps = OBS_ENCODER_CAP_DYN_BITRATE | OBS_ENCODER_CAP_INTERNAL,
#else
	.caps = OBS_ENCODER_CAP_DYN_BITRATE,
#endif
};

struct obs_encoder_info ffmpeg_amf_hevc_encoder_info = {
	.id = "h265_ffmpeg_amf",
	.type = OBS_ENCODER_VIDEO,
	.codec = "hevc",
	.get_name = ffmpeg_amf_hevc_getname,
	.create = ffmpeg_amf_hevc_create,
	.destroy = ffmpeg_amf_destroy,
	.encode = ffmpeg_amf_encode,
	.update = ffmpeg_amf_reconfigure,
	.get_defaults = amf_defaults,
	.get_properties = amf_hevc_properties,
	.get_extra_data = ffmpeg_amf_extra_data,
	.get_video_info = ffmpeg_amf_video_info,
#ifdef _WIN32
	.caps = OBS_ENCODER_CAP_DYN_BITRATE | OBS_ENCODER_CAP_INTERNAL,
#else
	.caps = OBS_ENCODER_CAP_DYN_BITRATE,
#endif
};