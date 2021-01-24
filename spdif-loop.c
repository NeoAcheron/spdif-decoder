#define _GNU_SOURCE

#include <math.h>
#include <stdio.h>
#include <unistd.h>
#include <err.h>
#include <getopt.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <ao/ao.h>
#include <libavformat/avio.h>

#include "resample.h"
#include "helper.h"
#include "myspdif.h"
#include "codechandler.h"

#define IO_BUFFER_SIZE ((8 + 1792 + 4344) * 1)

struct alsa_read_state
{
	AVFormatContext *ctx;
	AVPacket pkt;
	int offset;
};

static int debug_data;

static void
usage(void)
{
	fprintf(stderr,
			"usage:\n"
			"  spdif-loop [-t | -i <hw:alsa-input-dev>] -d <alsa|pulse> -o <surround-output-dev> [-p <stereo-passthrough-dev>]\n");
	exit(1);
}

static int
alsa_reader(void *data, uint8_t *buf, int buf_size)
{
	struct alsa_read_state *st = data;
	int read_size = 0;

	while (buf_size > 0)
	{
		if (st->pkt.size <= 0)
		{
			int ret = av_read_frame(st->ctx, &st->pkt);
			st->offset = 0;

			if (ret != 0)
				return (ret);
		}

		int pkt_left = st->pkt.size - st->offset;
		int datsize = buf_size < pkt_left ? buf_size : pkt_left;

		memcpy(buf, st->pkt.data + st->offset, datsize);
		st->offset += datsize;
		read_size += datsize;
		buf += datsize;
		buf_size -= datsize;

		if (debug_data)
		{
			static int had_zeros = 0;
			int i;
			for (i = 0; i < read_size; ++i)
			{
				const char zeros[16] = {0};
				if (i % 16 == 0 && read_size - i >= 16 &&
					memcmp((char *)buf + i, zeros, 16) == 0)
				{
					i += 15;
					if (had_zeros && had_zeros % 10000 == 0)
						printf("  (%d)\n", had_zeros * 16);
					if (!had_zeros)
						printf("...\n");
					had_zeros++;
					continue;
				}
				if (had_zeros)
					printf("  (%d)\n", had_zeros * 16);
				had_zeros = 0;
				printf("%02x%s", ((unsigned char *)buf)[i],
					   (i + 1) % 16 == 0 ? "\n" : " ");
			}
		}

		if (st->offset >= st->pkt.size)
			av_free_packet(&st->pkt);
	}

	return (read_size);
}

int main(int argc, char **argv)
{
	int opt_test = 0;
	char *alsa_dev_name = NULL;
	char *out_driver_name = NULL;
	char *out_dev_name = NULL;
	char *out_dev_passthrough_name = NULL;
	int opt;
	for (opt = 0; (opt = getopt(argc, argv, "d:hi:o:p:tv")) != -1;)
	{
		switch (opt)
		{
		case 'd':
			out_driver_name = optarg;
			break;
		case 'i':
			alsa_dev_name = optarg;
			break;
		case 'o':
			out_dev_name = optarg;
			break;
		case 'p':
			out_dev_passthrough_name = optarg;
			break;
		case 't':
			opt_test = 1;
			break;
		case 'v':
			debug_data = 1;
			break;
		default:
			usage();
			/* NOTREACHED */
		}
	}
	argc -= optind;
	argv += optind;

	if (argc != 0)
		usage();

	if (!(opt_test ^ !!alsa_dev_name))
	{
		fprintf(stderr, "please specify either input device or testing mode\n\n");
		usage();
	}

	av_register_all();
	avcodec_register_all();
	avdevice_register_all();
	ao_initialize();

	ao_option *out_dev_opts = NULL;
	if (out_dev_name)
	{
		if (!ao_append_option(&out_dev_opts, "dev", out_dev_name))
			errx(1, "cannot set output device `%s'", out_dev_name);
	}

	ao_option *out_dev_passthrough_opts = NULL;
	if (out_dev_passthrough_name)
	{
		if (!ao_append_option(&out_dev_passthrough_opts, "dev", out_dev_passthrough_name))
			errx(1, "cannot set passthrough device `%s'", out_dev_passthrough_name);
	}

	int out_driver_id = ao_default_driver_id();
	if (out_driver_name)
		out_driver_id = ao_driver_id(out_driver_name);
	if (out_driver_id < 0)
		errx(1, "invalid output driver `%s'",
			 out_driver_name ? out_driver_name : "default");

	if (opt_test)
	{
		exit(test_audio_out(out_driver_id, out_dev_opts));
		// NOTREACHED
	}

	AVInputFormat *alsa_fmt = av_find_input_format("alsa");
	if (!alsa_fmt)
		errx(1, "cannot find alsa input driver");

	AVInputFormat *spdif_fmt = av_find_input_format("spdif");
	if (!spdif_fmt)
		errx(1, "cannot find S/PDIF demux driver");

	const int alsa_buf_size = IO_BUFFER_SIZE;
	unsigned char *alsa_buf = av_malloc(alsa_buf_size);
	if (!alsa_buf)
		errx(1, "cannot allocate input buffer");

	AVFormatContext *spdif_ctx = NULL;
	AVFormatContext *alsa_ctx = NULL;
	ao_device *out_dev = NULL;

	uint8_t *audio_frame_data_pu8 = malloc(1 * 1024 * 1024);
	uint32_t audio_frame_size_pu32 = 0;

	if (0)
	{
	retry:
		if (spdif_ctx)
			avformat_close_input(&spdif_ctx);
		if (alsa_ctx)
			avformat_close_input(&alsa_ctx);
		if (out_dev)
		{
			ao_close(out_dev);
			out_dev = NULL;
		}
		sleep(1);
		printf("retrying.\n");
	}

	spdif_ctx = avformat_alloc_context();
	if (!spdif_ctx)
		errx(1, "cannot allocate S/PDIF context");

	if (avformat_open_input(&alsa_ctx, alsa_dev_name, alsa_fmt, NULL) != 0)
		errx(1, "cannot open alsa input");

	struct alsa_read_state read_state = {
		.ctx = alsa_ctx,
	};

	av_init_packet(&read_state.pkt);
	AVIOContext *avio_ctx = avio_alloc_context(alsa_buf, alsa_buf_size, 0, &read_state, alsa_reader, NULL, NULL);
	if (!avio_ctx)
	{
		errx(1, "cannot open avio_alloc_context");
	}

	spdif_ctx->pb = avio_alloc_context(alsa_buf, alsa_buf_size, 0, &read_state, alsa_reader, NULL, NULL);
	if (!spdif_ctx->pb)
		errx(1, "cannot set up alsa reader");

	if (avformat_open_input(&spdif_ctx, "internal", spdif_fmt, NULL) != 0)
		errx(1, "cannot open S/PDIF input");

	av_dump_format(alsa_ctx, 0, alsa_dev_name, 0);

	AVPacket pkt = {.size = 0, .data = NULL};
	av_init_packet(&pkt);

	CodecHandler codec_handler_st;
	CodecHandler_init(&codec_handler_st);
	printf("start loop\n");
	while (1)
	{
		// Read data from the SPDIF stream
		int r = my_spdif_read_packet(spdif_ctx, &pkt, audio_frame_data_pu8, IO_BUFFER_SIZE, &audio_frame_size_pu32);

		// If the read returns zero, it means a codec was found and the audio can be played from the packet
		if (r == 0)
		{
			if (CodecHandler_loadCodec(&codec_handler_st, spdif_ctx) != 0)
			{
				printf("Could not load codec %s.\n", avcodec_get_name(codec_handler_st.currentCodecID));
				goto retry;
			}

			if (CodecHandler_decodeCodec(&codec_handler_st, &pkt, audio_frame_data_pu8, &audio_frame_size_pu32) == 1)
			{
				// Force the output device to re-initialize on the next audio frame, in order to match the new codec parameters
				if (out_dev)
				{
					ao_close(out_dev);
					out_dev = NULL;
				}
				printf("Detected S/PDIF codec %s\n", avcodec_get_name(codec_handler_st.currentCodecID));
			}
			if (pkt.size != 0)
			{
				// This is generally a packet error. All data contained in the packet sould have been decoded
				printf("still some bytes left %d\n", pkt.size);
			}
		}
		else // The data read from the stream didn't have any header information, so we're going to assume its raw uncompressed stereo PCM data
		{
			if (codec_handler_st.currentCodecID != AV_CODEC_ID_NONE ||
				codec_handler_st.currentChannelCount != 2 ||
				codec_handler_st.currentSampleRate != 48000)
			{
				printf("Detected S/PDIF uncompressed audio\n");

				// Force the output device to re-initialize on the next audio frame, in order to match the new codec parameters
				if (out_dev)
				{
					ao_close(out_dev);
					out_dev = NULL;
				}
			}
			codec_handler_st.currentCodecID = AV_CODEC_ID_NONE;
			codec_handler_st.currentChannelCount = 2;
			codec_handler_st.currentSampleRate = 48000;
			codec_handler_st.currentChannelLayout = 0;
		}

		// Initialize the output device
		if (!out_dev)
		{
			if (out_dev_passthrough_opts && codec_handler_st.currentCodecID == AV_CODEC_ID_NONE)
			{
				printf("Using passthrough output device: %s\n", out_dev_passthrough_name);
				out_dev = open_output(out_driver_id,
									out_dev_passthrough_opts,
									av_get_bytes_per_sample(AV_SAMPLE_FMT_S16) * 8,
									codec_handler_st.currentChannelCount,
									codec_handler_st.currentSampleRate);
			}
			else
			{
				printf("Using primary output device: %s\n", out_dev_name);
				out_dev = open_output(out_driver_id,
									out_dev_opts,
									av_get_bytes_per_sample(AV_SAMPLE_FMT_S16) * 8,
									codec_handler_st.currentChannelCount,
									codec_handler_st.currentSampleRate);
			}
			if (!out_dev)
				errx(1, "cannot open audio output");
		}

		// Play the audio frame
		if (!ao_play(out_dev, audio_frame_data_pu8, audio_frame_size_pu32))
		{
			printf("Could not play audio to output device...");
			goto retry;
		}
	}
	CodecHandler_deinit(&codec_handler_st);
	return (0);
}
