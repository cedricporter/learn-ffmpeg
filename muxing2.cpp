#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <iostream>

#ifdef __cplusplus
extern "C" {
#endif
    
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>
#include <libavutil/timestamp.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

#ifdef __cplusplus
}
#endif

#define STREAM_FRAME_RATE 25
#define STREAM_DURATION   5.0
#define STREAM_PIX_FMT    AV_PIX_FMT_YUV420P /* default pix_fmt */


class VideoEncoder
{
public:
    VideoEncoder()
    {
        filename = "sample.mp4";

        samples_count = 0;
        dst_samples_data = 0;
        dst_samples_linesize = 0;
        dst_samples_size = 0;

        // video output
        frame = 0;
        frame_count = 0;

        video_is_eof = 0;
        audio_is_eof = 0;

        // audio output
        audio_frame = 0;
        src_samples_data = 0;
        src_samples_linesize = 0;
        src_nb_samples = 0;

        t = tincr = tincr2 = 0;

        max_dst_nb_samples = 0;
    
        swr_ctx = NULL;

        audio_st = NULL;
        video_st = NULL;
        audio_codec = NULL;
        video_codec = NULL;;

        fmt = NULL;
        oc = NULL;
    
        audio_time = video_time = 0;
    }

    void run()
    {
        // write data
        flush = 0;
        while ((video_st && !video_is_eof) || (audio_st && !audio_is_eof))
        {
            audio_time = (audio_st && !audio_is_eof) ?
                audio_st->pts.val * av_q2d(audio_st->time_base) : INFINITY;
            video_time = (video_st && !video_is_eof) ?
                video_st->pts.val * av_q2d(video_st->time_base) : INFINITY;
        
            printf("audio_time: %lf, video_time: %lf\n", audio_time, video_time);

            if (!flush
                && (!video_st || video_time >= STREAM_DURATION)
                && (!audio_st || audio_time >= STREAM_DURATION)
                ) {
                flush = 1;
            }

            if (audio_st && !audio_is_eof && audio_time <= video_time) {
                write_audio_frame(oc, audio_st, flush);
            }
            else if (video_st && !video_is_eof && video_time < audio_time) {
                write_video_frame(oc, video_st, flush);
            }
        }

    }
    
    ~VideoEncoder()
    {
        // write trailer
        av_write_trailer(oc);

        if (video_st) {
            close_video(oc, video_st);
        }
        if (audio_st) {
            close_audio(oc, audio_st);
        }

        if (!(fmt->flags & AVFMT_NOFILE)) {
            avio_close(oc->pb);
        }

        avformat_free_context(oc);
    }
    
    int InitEncoder(const std::string& filename, int width, int height, 
        int bitRate, int fps);

    int Feed(uint8_t* pixels, int width, int height, int64_t presentationTime)
    {
        return 0;
    }
    
    uint8_t* GetPixelsBuffer();

private:
    int DestroyEncoder();
    
/* Add an output stream. */
    AVStream *add_stream(AVFormatContext *oc, AVCodec **codec,
        AVCodecID codec_id)
    {
        AVCodecContext *c;
        AVStream *st;


        /* find the encoder */
        *codec = avcodec_find_encoder(codec_id);
        if (!(*codec)) {
            fprintf(stderr, "Could not find encoder for '%s'\n",
                avcodec_get_name(codec_id));
            exit(1);
        }

        /* Add a new stream to a media file.
         * Returns newly created stream or NULL on error.
         */
        printf("1. oc->nb_streams: %d\n", oc->nb_streams);
        st = avformat_new_stream(oc, *codec);
        printf("2. oc->nb_streams: %d\n", oc->nb_streams);

        if (!st) {
            fprintf(stderr, "Could not allocate stream\n");
            exit(1);
        }
        st->id = oc->nb_streams-1;  /* A list of all streams in the file. */
        c = st->codec;

        switch ((*codec)->type) {
        case AVMEDIA_TYPE_AUDIO:
            c->sample_fmt  = (*codec)->sample_fmts ?
                (*codec)->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
            c->bit_rate    = 64000;
            c->sample_rate = 44100;
            c->channels    = 2;
            break;

        case AVMEDIA_TYPE_VIDEO:
            c->codec_id = codec_id;

            c->bit_rate = 400000;
            /* Resolution must be a multiple of two. */
            c->width    = 800;
            c->height   = 480;
            /* timebase: This is the fundamental unit of time (in seconds) in terms
             * of which frame timestamps are represented. For fixed-fps content,
             * timebase should be 1/framerate and timestamp increments should be
             * identical to 1. */
            c->time_base.den = STREAM_FRAME_RATE;
            c->time_base.num = 1;
            c->gop_size      = 12; /* emit one intra frame every twelve frames at most */
            c->pix_fmt       = STREAM_PIX_FMT;
            if (c->codec_id == AV_CODEC_ID_MPEG2VIDEO) {
                printf("c->codec_id == AV_CODEC_ID_MPEG2VIDEO\n");
                /* just for testing, we also add B frames */
                c->max_b_frames = 2;
            }
            if (c->codec_id == AV_CODEC_ID_MPEG1VIDEO) {
                printf("c->codec_id == AV_CODEC_ID_MPEG1VIDEO\n");
                /* Needed to avoid using macroblocks in which some coeffs overflow.
                 * This does not happen with normal video, it just happens here as
                 * the motion of the chroma plane does not match the luma plane. */
                c->mb_decision = 2;
            }
            break;

        default:
            break;
        }

        /* Some formats want stream headers to be separate. */
        if (oc->oformat->flags & AVFMT_GLOBALHEADER) {
            printf("oc->oformat->flags & AVFMT_GLOBALHEADER\n");
            c->flags |= CODEC_FLAG_GLOBAL_HEADER;
        }

        return st;
    }

    
    int write_frame(AVFormatContext *fmt_ctx, const AVRational *time_base,
        AVStream *st, AVPacket *pkt)
    {
        pkt->pts = av_rescale_q_rnd(pkt->pts, *time_base, st->time_base, (AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
        pkt->dts = av_rescale_q_rnd(pkt->dts, *time_base, st->time_base, (AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
        pkt->duration = av_rescale_q(pkt->duration, *time_base, st->time_base);
        pkt->stream_index = st->index;

        return av_interleaved_write_frame(fmt_ctx, pkt);
    }

    
/* Prepare a dummy image. */
    void fill_yuv_image(AVPicture *pict, int frame_index,
        int width, int height)
    {
        int x, y, i;

        i = frame_index;

        if (i > 20) {
            /* Y */
            for (y = 0; y < height; y++)
                for (x = 0; x < width; x++)
                    pict->data[0][y * pict->linesize[0] + x] = x + y + i * 3;

            /* Cb and Cr */
            for (y = 0; y < height / 2; y++) {
                for (x = 0; x < width / 2; x++) {
                    pict->data[1][y * pict->linesize[1] + x] = 128 + y + i * 2;
                    pict->data[2][y * pict->linesize[2] + x] = 64 + x + i * 5;
                }
            }
        } else {
            for (y = 0; y < height; y++) {
                for (x = 0; x < width; x++)
                {
                    pict->data[0][y * pict->linesize[0] + x] = x + y + i * 3;
                }
            }
        }
    }

    
    void write_video_frame(AVFormatContext *oc, AVStream *st, int flush)
    {
        printf("write_video_frame frame: %d\n", frame_count);
    
        int ret;
        AVCodecContext *c = st->codec;

        if (!flush) {
            fill_yuv_image(&dst_picture, frame_count, c->width, c->height);
        }

        AVPacket pkt = { 0 };
        int got_packet;
        av_init_packet(&pkt);

        frame->pts = frame_count;
        ret = avcodec_encode_video2(c, &pkt, flush ? NULL : frame, &got_packet);
        if (ret < 0) {
            printf("avcodec_encode_video2 failed %s\n", av_err2str(ret));
            exit(1);
        }

        if (got_packet) {
            ret = write_frame(oc, &c->time_base, st, &pkt);
        } else {
            if (flush) {
                video_is_eof = 1;
            }
            ret = 0;
        }

        if (ret < 0) {
            printf("write frame failed %s\n", av_err2str(ret));
            exit(1);
        }

        frame_count++;
    }

    void open_audio(AVFormatContext *oc, AVCodec *codec, AVStream *st)
    {
        AVCodecContext *c;
        int ret;

        c = st->codec;

        /* allocate and init a re-usable frame */
        audio_frame = av_frame_alloc();
        if (!audio_frame) {
            fprintf(stderr, "Could not allocate audio frame\n");
            exit(1);
        }

        c->strict_std_compliance = -2;

        /* open it */
        ret = avcodec_open2(c, codec, NULL);
        if (ret < 0) {
            fprintf(stderr, "Could not open audio codec: %s\n", av_err2str(ret));
            exit(1);
        }

        /* init signal generator */
        t     = 0;
        tincr = 2 * M_PI * 110.0 / c->sample_rate;
        /* increment frequency by 110 Hz per second */
        tincr2 = 2 * M_PI * 110.0 / c->sample_rate / c->sample_rate;

        src_nb_samples = c->codec->capabilities & CODEC_CAP_VARIABLE_FRAME_SIZE ?
            10000 : c->frame_size;

        /* Allocate a data pointers array, samples buffer for nb_samples samples,
           and fill data pointers and linesize accordingly. */
        ret = av_samples_alloc_array_and_samples(&src_samples_data, &src_samples_linesize, c->channels,
            src_nb_samples, AV_SAMPLE_FMT_S16, 0);
        if (ret < 0) {
            fprintf(stderr, "Could not allocate source samples\n");
            exit(1);
        }

        /* compute the number of converted samples: buffering is avoided
         * ensuring that the output buffer will contain at least all the
         * converted input samples */
        max_dst_nb_samples = src_nb_samples;

        /* create resampler context */
        if (c->sample_fmt != AV_SAMPLE_FMT_S16) {
            swr_ctx = swr_alloc();
            if (!swr_ctx) {
                fprintf(stderr, "Could not allocate resampler context\n");
                exit(1);
            }

            /* set options */
            av_opt_set_int       (swr_ctx, "in_channel_count",   c->channels,       0);
            av_opt_set_int       (swr_ctx, "in_sample_rate",     c->sample_rate,    0);
            av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt",      AV_SAMPLE_FMT_S16, 0);
            av_opt_set_int       (swr_ctx, "out_channel_count",  c->channels,       0);
            av_opt_set_int       (swr_ctx, "out_sample_rate",    c->sample_rate,    0);
            av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt",     c->sample_fmt,     0);

            /* initialize the resampling context */
            if ((ret = swr_init(swr_ctx)) < 0) {
                fprintf(stderr, "Failed to initialize the resampling context\n");
                exit(1);
            }

            ret = av_samples_alloc_array_and_samples(&dst_samples_data, &dst_samples_linesize, c->channels,
                max_dst_nb_samples, c->sample_fmt, 0);
            if (ret < 0) {
                fprintf(stderr, "Could not allocate destination samples\n");
                exit(1);
            }
        } else {
            dst_samples_data = src_samples_data;
        }
        dst_samples_size = av_samples_get_buffer_size(NULL, c->channels, max_dst_nb_samples,
            c->sample_fmt, 0);
    }

    void open_video(AVFormatContext *oc, AVCodec *codec, AVStream *st)
    {
        int ret;
        AVCodecContext *c = st->codec;

        ret = avcodec_open2(c, codec, NULL);
        if (ret < 0) {
            printf("avcodec_open2(c, codec, NULL); failed: %s\n", av_err2str(ret));
            exit(1);
        }

        // create a re-usable frame
        frame = av_frame_alloc();
        if (!frame) {
            printf("av_frame_alloc() failed %s\n", av_err2str(ret));
            exit(1);
        }
        frame->format = c->pix_fmt;
        frame->width = c->width;
        frame-> height = c->height;
    
        /* Allocate the encoded raw picture. */
        ret = avpicture_alloc(&dst_picture, c->pix_fmt, c->width, c->height);
        if (ret < 0) {
            printf("avpicture_alloc(&dst_picture, c->pix_fmt, c->width, c->height); failed: %s\n", av_err2str(ret));
            exit(1);
        }

        *((AVPicture *)frame) = dst_picture;
    }

    
    void close_video(AVFormatContext *oc, AVStream *st)
    {
        avcodec_close(st->codec);
        av_free(dst_picture.data[0]);
        av_frame_free(&frame);
    }

    void write_audio_frame(AVFormatContext *oc, AVStream *st, int flush)
    {
        AVCodecContext *c;
        AVPacket pkt = { 0 }; // data and size must be 0;
        int got_packet, ret, dst_nb_samples;

        av_init_packet(&pkt);
        c = st->codec;

        if (!flush) {
            get_audio_frame((int16_t *)src_samples_data[0], src_nb_samples, c->channels);

            /* convert samples from native format to destination codec format, using the resampler */
            if (swr_ctx) {
                /* compute destination number of samples */
                dst_nb_samples = av_rescale_rnd(swr_get_delay(swr_ctx, c->sample_rate) + src_nb_samples,
                    c->sample_rate, c->sample_rate, AV_ROUND_UP);
                if (dst_nb_samples > max_dst_nb_samples) {
                    av_free(dst_samples_data[0]);
                    ret = av_samples_alloc(dst_samples_data, &dst_samples_linesize, c->channels,
                        dst_nb_samples, c->sample_fmt, 0);
                    if (ret < 0)
                        exit(1);
                    max_dst_nb_samples = dst_nb_samples;
                    dst_samples_size = av_samples_get_buffer_size(NULL, c->channels, dst_nb_samples,
                        c->sample_fmt, 0);
                }

                /* convert to destination format */
                ret = swr_convert(swr_ctx,
                    dst_samples_data, dst_nb_samples,
                    (const uint8_t **)src_samples_data, src_nb_samples);
                if (ret < 0) {
                    fprintf(stderr, "Error while converting\n");
                    exit(1);
                }
            } else {
                dst_nb_samples = src_nb_samples;
            }

            audio_frame->nb_samples = dst_nb_samples;
            audio_frame->pts = av_rescale_q(samples_count, (AVRational){1, c->sample_rate}, c->time_base);
            avcodec_fill_audio_frame(audio_frame, c->channels, c->sample_fmt,
                dst_samples_data[0], dst_samples_size, 0);
            samples_count += dst_nb_samples;
        }

        ret = avcodec_encode_audio2(c, &pkt, flush ? NULL : audio_frame, &got_packet);
        if (ret < 0) {
            fprintf(stderr, "Error encoding audio frame: %s\n", av_err2str(ret));
            exit(1);
        }

        if (!got_packet) {
            if (flush)
                audio_is_eof = 1;
            return;
        }

        ret = write_frame(oc, &c->time_base, st, &pkt);
        if (ret < 0) {
            fprintf(stderr, "Error while writing audio frame: %s\n",
                av_err2str(ret));
            exit(1);
        }
    }

    
    /* Prepare a 16 bit dummy audio frame of 'frame_size' samples and
     * 'nb_channels' channels. */
    void get_audio_frame(int16_t *samples, int frame_size, int nb_channels)
    {
        int j, i, v;
        int16_t *q;

        q = samples;
        for (j = 0; j < frame_size; j++) {
            v = (int)(sin(t) * 10000);
            for (i = 0; i < nb_channels; i++)
                *q++ = v;
            t     += tincr;
            tincr += tincr2;
        }
    }

    void close_audio(AVFormatContext *oc, AVStream *st)
    {
        avcodec_close(st->codec);
        if (dst_samples_data != src_samples_data) {
            av_free(dst_samples_data[0]);
            av_free(dst_samples_data);
        }
        av_free(src_samples_data[0]);
        av_free(src_samples_data);
        av_frame_free(&audio_frame);
    }


private:
    // video output
    AVFrame *frame;
    AVPicture src_picture, dst_picture;
    int frame_count;

    int video_is_eof, audio_is_eof;;

    // audio output
    AVFrame *audio_frame;
    uint8_t **src_samples_data;
    int src_samples_linesize;
    int src_nb_samples;

    float t, tincr, tincr2;

    int max_dst_nb_samples;
    uint8_t **dst_samples_data;
    int dst_samples_linesize;
    int dst_samples_size;
    int samples_count;

    SwrContext *swr_ctx;

    AVStream *audio_st, *video_st;
    AVCodec *audio_codec, *video_codec;

    const char *filename;
    AVOutputFormat *fmt;
    AVFormatContext *oc;
    double audio_time, video_time;
    int flush, ret;

public:
    int init()
    {
        av_register_all();

        avformat_alloc_output_context2(&oc, NULL, NULL, filename);
        if (!oc)
        {
            return 1;
        }

        fmt = oc->oformat;

        video_st = NULL;
        audio_st = NULL;

        printf("AV_CODEC_ID_H264: %d\n", AV_CODEC_ID_H264);
        printf("fmt->video_codec: %d\n", fmt->video_codec);  // AV_CODEC_ID_MPEG1VIDEO
        printf("fmt->audio_codec: %d\n", fmt->audio_codec);  // AV_CODEC_ID_MP2

        // add stream
        if (fmt->video_codec != AV_CODEC_ID_NONE) 
            video_st = add_stream(oc, &video_codec, fmt->video_codec);
        if (fmt->audio_codec != AV_CODEC_ID_NONE)
            audio_st = add_stream(oc, &audio_codec, fmt->audio_codec);

        // open video codec and allocate buffer
        if (video_st) {
            printf("open_video\n");
            open_video(oc, video_codec, video_st);
        }
        if (audio_st) {
            printf("open_audio\n");
            open_audio(oc, audio_codec, audio_st);
        }
    
        av_dump_format(oc, 0, filename, 1);

        // open file
        if (!(fmt->flags & AVFMT_NOFILE)) {
            ret = avio_open(&oc->pb, filename, AVIO_FLAG_WRITE);
            if (ret < 0) {
                printf("open file failed\n");
                return 1;
            }
        }

        // write the stream header
        ret = avformat_write_header(oc, NULL);
        if (ret < 0) {
            printf("write header failed\n");
            return 1;
        }
    
        return 0;
    }

};

int main(int argc, char *argv[])
{
    std::cout << "Starting" << std::endl;
    
    {
        VideoEncoder encoder;
        std::cout << "encoder.init: " << encoder.init() << std::endl;
        encoder.run();
    }
    
    printf("DONE\n");
    return 0;
}
