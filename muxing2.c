#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <libavutil/opt.h>
#include <libavutil/mathematics.h>
#include <libavutil/timestamp.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

#define STREAM_FRAME_RATE 25
#define STREAM_DURATION   10.0
#define STREAM_PIX_FMT    AV_PIX_FMT_YUV420P /* default pix_fmt */


// video output
static AVFrame *frame;
static AVPicture src_picture, dst_picture;
static int frame_count;

static int video_is_eof;

static AVStream *add_stream(AVFormatContext *oc, AVCodec **codec, enum AVCodecID codec_id)
{
    AVCodecContext *c;
    AVStream *st;

    printf("avcodec_find_encoder\n");
    *codec = avcodec_find_encoder(codec_id);
    if (!(*codec)) {
        printf("avcodec_find_encoder(codec_id) failed\n");
        exit(1);
    }

    // Add a new stream to a mdeia file.
    printf("avformat_new_stream(oc, *codec)\n");
    printf("1. oc->nb_streams: %d\n", oc->nb_streams);
    st = avformat_new_stream(oc, *codec);
    printf("2. oc->nb_streams: %d\n", oc->nb_streams);
    
    if (!st) {
        printf("avformat_new_stream(oc, *codec); failed\n");
        exit(1);
    }

    printf("2 avformat_new_stream(oc, *codec)\n");
    
    st->id = oc->nb_streams - 1;
    c = st->codec;

    c->codec_id = codec_id;
    c->bit_rate = 40 * 10000;
    c->width = 800;
    c->height = 600;

    c->time_base.den = STREAM_FRAME_RATE;
    c->time_base.num = 1;
    c->gop_size = 12;
    c->pix_fmt = STREAM_PIX_FMT;

    if (oc->oformat->flags & AVFMT_GLOBALHEADER) {
        c->flags |= CODEC_FLAG_GLOBAL_HEADER;
    }

    return st;
}

/* Prepare a dummy image. */
static void fill_yuv_image(AVPicture *pict, int frame_index,
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

static int write_frame(AVFormatContext *fmt_ctx, const AVRational *time_base, AVStream *st, AVPacket *pkt)
{
    pkt->pts = av_rescale_q_rnd(pkt->pts, *time_base, st->time_base, AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
    pkt->dts = av_rescale_q_rnd(pkt->dts, *time_base, st->time_base, AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
    pkt->duration = av_rescale_q(pkt->duration, *time_base, st->time_base);
    pkt->stream_index = st->index;

    return av_interleaved_write_frame(fmt_ctx, pkt);
}

static void write_video_frame(AVFormatContext *oc, AVStream *st, int flush)
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

static void open_video(AVFormatContext *oc, AVCodec *codec, AVStream *st)
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

static void close_video(AVFormatContext *oc, AVStream *st)
{
    avcodec_close(st->codec);
    av_free(dst_picture.data[0]);
    av_frame_free(&frame);
}

int main(int argc, char *argv[])
{
    AVStream *audio_st, *video_st;
    AVCodec *audio_codec, *video_codec;

    AVFormatContext *oc;
    AVOutputFormat *fmt;
    char filename[] = "sample.mp4";
    int flush, ret;

    double audio_time, video_time;

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
    video_st = add_stream(oc, &video_codec, fmt->video_codec);

    // open video codec and allocate buffer
    open_video(oc, video_codec, video_st);

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

    // write data
    flush = 0;
    while ((video_st && !video_is_eof)) {
        video_time = (video_st && !video_is_eof) ? video_st->pts.val * av_q2d(video_st->time_base) : INFINITY;
        
        printf("video_time: %lf\n", video_time);

        if (!flush &&
            (!video_st || video_time >= STREAM_DURATION)) {
            flush = 1;
        }

        if (video_st && !video_is_eof) {
            write_video_frame(oc, video_st, flush);
        }
    }

    // write trailer
    av_write_trailer(oc);

    if (video_st) {
        close_video(oc, video_st);
    }

    if (!(fmt->flags & AVFMT_NOFILE)) {
        avio_close(oc->pb);
    }

    avformat_free_context(oc);

    printf("DONE\n");
    return 0;
}
