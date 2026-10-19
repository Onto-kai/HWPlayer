extern "C"{
    #include <libswresample/swresample.h>
    #include <libavutil/hwcontext.h>
    #include <libavutil/pixdesc.h>
    #include <libswscale/swscale.h>
}

#include "packet_queue.h"
#include "frame_queue.h"
#include "video_player.h"
#include "audio_player.h"
#include "ring_buffer.h"
#include "shader.hpp"
#include "tools.h"

//#include <SDL2/SDL.h>
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <thread>
#include <atomic>
#include <iostream>
#include <string>
//#include </usr/local/cuda/targets/x86_64-linux/include/cuda_gl_interop.h>
#include <cuda_gl_interop.h>
#include <algorithm>


/*
    快进后退
*/
static const double TIME_INTERVAL = 5.0;
std::atomic<bool> seek_request(false);
std::atomic<bool> seek_done(false);
std::atomic<bool> seek_pending(false);
int64_t seek_pos = 0;

/*
    音视频同步
*/
//用于平滑修正
double audio_sync_offset = 0.0;
//上一次校验时间
Uint32 last_sync_check_ticks = 0;
//同步校验间隔
const Uint32 SYNC_CHECK_INTERVAL = 100;
//最大修正幅度
const double MAX_STEP_ADJUST = 0.002;
//丢包阈值
const double BIG_DIFF_TIMESOUT = 0.06;


/*
    屏幕宽高
*/
static const double SCREEN_W = 2048.0;
static const double SCREEN_H = 1536.0;

constexpr double dpi_scale = 1.5;


//文件大小
double file_duration;

static const int SDL_samples = 2048;


static std::atomic<bool> g_running(true);
std::atomic<bool> quit(false);

struct PlayerState {
    AVFormatContext *fmtCtx = nullptr;
    AVCodecContext *vCtx = nullptr;
    AVCodecContext *aCtx = nullptr;

    SwrContext *swrCtx = nullptr;
    SwsContext *swsCtx = nullptr;

    SDL_AudioDeviceID audioDev = 0;

    Packet_queue vpq;
    Packet_queue apq;
    Frame_queue vfq;

    int vIdx = -1;
    int aIdx = -1;

    AVRational vTimeBase;
    AVRational aTimeBase;

    std::atomic<double> audio_clock{0.0};

    int max_samples = 0;

    int bytes_per_sample = 0;

    std::atomic<bool> eof;

};

enum Player_Status{
    PAUSE,
    PLAY
};

std::atomic<enum Player_Status> Status(PLAY);

Ring_buffer rbuf = {};

int video_seek(PlayerState *st);

int readThread(PlayerState *st)
{
    AVPacket *pkt = av_packet_alloc();
    if(!pkt) return -1;

    while(g_running){

        if(Status == PAUSE){
            SDL_Delay(4);
            continue;
        }

        if(seek_request){
            video_seek(st);
            seek_request = false;
            seek_done = true;
        }

        int ret = av_read_frame(st->fmtCtx, pkt);
        
        if(ret < 0) {
            if(ret == AVERROR_EOF){
                st->eof = true;
                continue;
            }
            break;
        }

        if(pkt->stream_index == st->vIdx){
            st->vpq.push(pkt);
        }else if(pkt->stream_index == st->aIdx){
            st->apq.push(pkt);
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);

    st->vpq.push(nullptr);
    st->apq.push(nullptr);
    return 0;
}

int videoThread(PlayerState *st)
{
    AVFrame *frame = av_frame_alloc();
    if(!frame) return -1;
    double vpts = 0;

    while(g_running){

        if(Status == PAUSE){
            SDL_Delay(4);
            continue;
        }

        AVPacket *pkt = st->vpq.pop(g_running);
        if(!pkt) break;

        if(!pkt->data){ av_packet_free(&pkt); break; }

        int ret = avcodec_send_packet(st->vCtx, pkt);
        av_packet_free(&pkt);
        if(ret < 0) continue;

        while(true){
            ret = avcodec_receive_frame(st->vCtx, frame);
            if(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if(ret < 0) break;

            //+
            if(frame->pts != AV_NOPTS_VALUE){
                vpts = frame->pts * av_q2d(st->vTimeBase);
                if(seek_pending && vpts < seek_pos){
                    av_frame_unref(frame);
                    continue;
                }
            }
             st->vfq.push(frame);
             av_frame_unref(frame);
       }
    }

    avcodec_send_packet(st->vCtx, nullptr);
    while(Vdecode(st->vCtx, frame, nullptr) == 1){
        st->vfq.push(frame);
        av_frame_unref(frame);
    }

    av_frame_free(&frame);
    return 0;
}

int audioThread(PlayerState *st)
{
    AVFrame *frame = av_frame_alloc();
    if(!frame){ return -1; }

    double apts = 0;

    uint8_t *buf = nullptr;
    st->bytes_per_sample = av_get_bytes_per_sample(st->aCtx->sample_fmt);

    int ringbuf_size = 4 * SDL_samples * st->aCtx->ch_layout.nb_channels * st->bytes_per_sample;
    //rbuf.resize(ringbuf_size);

    while(g_running){

        if(Status == PAUSE){
            SDL_Delay(4);
            continue;
        }

        AVPacket *pkt = st->apq.pop(g_running);
        if(!pkt) break;

        if(!pkt->data){ av_packet_free(&pkt); break; }

        int ret = avcodec_send_packet(st->aCtx, pkt);
        av_packet_free(&pkt);
        if(ret < 0) continue;

        while(true){
            ret = avcodec_receive_frame(st->aCtx, frame);
            if(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if(ret < 0) break;

            if(frame->pts != AV_NOPTS_VALUE){
                apts = frame->pts * av_q2d(st->aTimeBase);

                if(seek_pending && apts < seek_pos){
                    av_frame_unref(frame);
                    continue;
                }
            }

            st->max_samples = av_rescale_rnd(
                swr_get_delay(st->swrCtx,st->aCtx->sample_rate) + frame->nb_samples,
                st->aCtx->sample_rate,
                st->aCtx->sample_rate,
                AV_ROUND_UP
            );

            buf = (uint8_t*)av_malloc(st->max_samples * st->aCtx->ch_layout.nb_channels * st->bytes_per_sample);
            if(!buf) return -1;

            int out = swr_convert(st->swrCtx, &buf, st->max_samples,
                        (const uint8_t **)frame->data, frame->nb_samples);
                    
            // if(out > 0){
            //     SDL_QueueAudio(st->audioDev, buf, out * st->aCtx->channels * st->bytes_per_sample);
            //     if(frame->pts != AV_NOPTS_VALUE){
            //         st->audio_clock.store(frame->pts * av_q2d(st->aTimeBase));
            //     }
            // }

            if(out > 0){
                ssize_t audio_byte_len = out * st->aCtx->ch_layout.nb_channels * st->bytes_per_sample;
                rbuf.RB_push(buf,audio_byte_len);
            }

            if(seek_pending){
                seek_pending = false;
            }
            
            av_free(buf);
            av_frame_unref(frame);
        }
    }

    st->max_samples = av_rescale_rnd(
        swr_get_delay(st->swrCtx,st->aCtx->sample_rate) + frame->nb_samples,
        st->aCtx->sample_rate,
        st->aCtx->sample_rate,
        AV_ROUND_UP
    );

    buf = (uint8_t *)av_malloc(st->max_samples * st->aCtx->ch_layout.nb_channels * st->bytes_per_sample);
    if(!buf){ return -1; }

    avcodec_send_packet(st->aCtx, nullptr);
    while(Adecode(st->aCtx, frame, nullptr) == 1){
        int out = swr_convert(st->swrCtx, &buf, st->max_samples,
                    (const uint8_t **)frame->data, frame->nb_samples);
        // if(out > 0){
        //     SDL_QueueAudio(st->audioDev, buf, out * 2 * sizeof(float));
        // }

        if(out > 0){
            ssize_t audio_byte_len = out * st->aCtx->ch_layout.nb_channels * st->bytes_per_sample;
            rbuf.RB_push(buf,audio_byte_len);
        }

        av_frame_unref(frame);
    }

    av_frame_free(&frame);
    if(buf){
        av_free(buf);
    }
    return 0;
}

double getAudioClock(PlayerState *st)
{
    return st->audio_clock;
}

enum AVPixelFormat get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts){
    const enum AVPixelFormat *p;
    for(p = pix_fmts; *p != AV_PIX_FMT_NONE; p++){
        if(*p == AV_PIX_FMT_CUDA){
            return *p;
        }
    }
    return AV_PIX_FMT_NONE;
}

void audio_callback(void *userdata, Uint8 *stream, int len){

    PlayerState *st = (PlayerState*)userdata;

    SDL_memset(stream,0,len);
    int read = rbuf.RB_cat(stream,len);

    if(read > 0){
        double seconds = read / (double)(st->aCtx->sample_rate * st->aCtx->ch_layout.nb_channels * st->bytes_per_sample);
        st->audio_clock = st->audio_clock + seconds;
    }

}


void HandleEvent(PlayerState *st, SDL_Event &event){
    if(event.type == SDL_QUIT) quit = true;

    if(event.type == SDL_KEYDOWN &&
       !event.key.repeat)
    {
        if(event.key.keysym.sym == SDLK_ESCAPE)
        {
            quit = true;
        }


        if(event.key.keysym.sym == SDLK_SPACE)
        {
            if(Status == PLAY)
            {
                Status = PAUSE;
                SDL_PauseAudio(1);
            }
            else
            {
                Status = PLAY;
                SDL_PauseAudio(0);
            }
        }

        if(event.key.keysym.sym == SDLK_LEFT){
            seek_pos = getAudioClock(st) - TIME_INTERVAL;
            if(seek_pos <= 0){
                seek_pos = 0;
            }
            std::cerr << "seek pos " << seek_pos << std::endl;
            seek_request = true;
        }

        if(event.key.keysym.sym == SDLK_RIGHT){
            seek_pos = getAudioClock(st) + TIME_INTERVAL;
            if(seek_pos >= file_duration){
                seek_pos = file_duration;
            }
            seek_request = true;
            std::cerr << "seek pos " << seek_pos << std::endl;
            //quit = true;
        }
    }
}

int video_seek(PlayerState *st){

    Status = PAUSE;

    SDL_PauseAudio(1);

    st->apq.flush();
    st->vpq.flush();
    st->vfq.flush();

    rbuf.flush();

    int64_t us = (int64_t)(seek_pos * AV_TIME_BASE);

    int ret =avformat_seek_file(st->fmtCtx,-1,INT_MIN,us,INT_MAX,AVSEEK_FLAG_BACKWARD);
    if(ret < 0){
        std::cerr << __LINE__ << " seek video failed " << avError(ret) << std::endl;
        return -1;
    }

    avformat_flush(st->fmtCtx);
    st->eof = false;

    if(ret >= 0){
        std::cerr << "function avformat seek file use sucess" << std::endl;
    }

    avcodec_flush_buffers(st->vCtx);
    avcodec_flush_buffers(st->aCtx);

    seek_pending = true;

    audio_sync_offset = 0.0;

    st->audio_clock = seek_pos;

    SDL_PauseAudio(0);

    Status = PLAY;

    return 0;
}


int main(int argc,const char *argv[])
{
    //std::cout << "FFmpeg libraries: "
    //    << av_version_info() << '\n';
    //return 0;

    SDL_SetMainReady();
    SDL_Event event;
    //硬件解码上下文
    AVBufferRef *hw_device_ctx = nullptr;

    if(argc < 2){
        std::cerr << "Usage: player <filename>" << std::endl;
        return -1;
    }
    //std::string video_path = "D://Users//Videos//AV//0.mov";

    //argv[1] = video_path.data();

    PlayerState st;

    st.eof = false;

    av_log_set_level(AV_LOG_WARNING);

    if(av_hwdevice_ctx_create(&hw_device_ctx,AV_HWDEVICE_TYPE_CUDA,nullptr,nullptr,0) < 0){
        av_buffer_unref(&hw_device_ctx);
        return -1;    
    }

    if(avformat_open_input(&st.fmtCtx, argv[1], nullptr, nullptr) < 0){
        std::cerr << "Cannot open " << argv[1] << std::endl;
        return -1;
    }
    if(avformat_find_stream_info(st.fmtCtx, nullptr) < 0){
        std::cerr << "Cannot find stream info" << std::endl;
        avformat_close_input(&st.fmtCtx);
        return -1;
    }

    if(st.fmtCtx->duration != AV_NOPTS_VALUE){
        file_duration = st.fmtCtx->duration / (double)AV_TIME_BASE;
        std::cerr << "file duration : " << file_duration << std::endl;
    }
    if(st.fmtCtx->duration == AV_NOPTS_VALUE){
        av_log(nullptr,AV_LOG_ERROR,"%d Cannot fast forward or rewind\n",__LINE__);
    }

    for(unsigned i = 0; i < st.fmtCtx->nb_streams; i++){
        if(st.fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && st.vIdx < 0)
            st.vIdx = i;
        if(st.fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && st.aIdx < 0)
            st.aIdx = i;
    }
    if(st.vIdx < 0 || st.aIdx < 0){
        std::cerr << "Need both video and audio stream" << std::endl;
        avformat_close_input(&st.fmtCtx);
        return -1;
    }

    std::cout << "fmtCtx codec_id = " << st.fmtCtx->streams[st.vIdx]->codecpar->codec_id << std::endl;
    //const AVCodec *vCodec = avcodec_find_decoder_by_name("h264_cuvid");

    const AVCodec* vCodec = nullptr;

    if (st.fmtCtx->streams[st.vIdx]->codecpar->codec_id == AV_CODEC_ID_H264) {
        vCodec = avcodec_find_decoder_by_name("h264_cuvid");
    }
    else if (st.fmtCtx->streams[st.vIdx]->codecpar->codec_id == AV_CODEC_ID_HEVC) {
        vCodec = avcodec_find_decoder_by_name("hevc_cuvid");
    }
    else {
        vCodec = avcodec_find_decoder(st.vCtx->codec_id);
    }

    if (!vCodec) {
        std::cerr << "Cannot find video decoder" << std::endl;
        return -1;
    }

    const AVCodec *aCodec = avcodec_find_decoder(st.fmtCtx->streams[st.aIdx]->codecpar->codec_id);
    if(!vCodec || !aCodec){
        std::cerr << "Decoder not found" << std::endl;
        avformat_close_input(&st.fmtCtx);
        return -1;
    }

    st.vCtx = avcodec_alloc_context3(vCodec);
    st.aCtx = avcodec_alloc_context3(aCodec);
    avcodec_parameters_to_context(st.vCtx, st.fmtCtx->streams[st.vIdx]->codecpar);
    avcodec_parameters_to_context(st.aCtx, st.fmtCtx->streams[st.aIdx]->codecpar);


    st.vCtx->hw_device_ctx = av_buffer_ref(hw_device_ctx);

    st.vCtx->get_format = get_hw_format;

    if(avcodec_open2(st.vCtx, vCodec, nullptr) < 0){
        std::cerr << "Cannot open video decoder" << std::endl;
        avcodec_free_context(&st.vCtx);
        avcodec_free_context(&st.aCtx);
        avformat_close_input(&st.fmtCtx);
        return -1;
    }
    if(avcodec_open2(st.aCtx, aCodec, nullptr) < 0){
        std::cerr << "Cannot open audio decoder" << std::endl;
        avcodec_free_context(&st.vCtx);
        avcodec_free_context(&st.aCtx);
        avformat_close_input(&st.fmtCtx);
        return -1;
    }

    st.vTimeBase = st.fmtCtx->streams[st.vIdx]->time_base;
    st.aTimeBase = st.fmtCtx->streams[st.aIdx]->time_base;

    int video_w = st.vCtx->width;
    int video_h = st.vCtx->height;

    int vw = st.vCtx->width;
    int vh = st.vCtx->height;

    //SDL_Rect dstrect = {};

    std::cout << "video_w = " << video_w << std::endl;
    std::cout << "video_h = " << video_h << std::endl;
    
    double ratio_v = static_cast<double>(video_w) / video_h;

    vw = video_w;
    vh = video_h;

    if (vw > SCREEN_W || vh > SCREEN_H) {
        double scale = std::min(
            static_cast<double>(SCREEN_W) / video_w,
            static_cast<double>(SCREEN_H) / video_h
        );

        vw = static_cast<int>(video_w * scale);
        vh = static_cast<int>(video_h * scale);
    }

    std::cout << "scale after vw = " << vw << std::endl;
    std::cout << "scale after vh = " << vh << std::endl;

    constexpr double dpi_scale = 1.5;

    int window_w = static_cast<int>(vw / dpi_scale);
    int window_h = static_cast<int>(vh / dpi_scale);

    //dstrect.x = 0;
    //dstrect.y = 0;

    //dstrect.w = vw;
    //dstrect.h = vh;

    AVChannelLayout out_ch_layout = AV_CHANNEL_LAYOUT_STEREO;

    int result = swr_alloc_set_opts2(&st.swrCtx,
        &out_ch_layout, AV_SAMPLE_FMT_FLT, st.aCtx->sample_rate,
        &st.aCtx->ch_layout, st.aCtx->sample_fmt, st.aCtx->sample_rate,
        0, nullptr);


    if(result < 0 || swr_init(st.swrCtx) < 0){
        std::cerr << "Cannot init resampler" << std::endl;
        av_channel_layout_uninit(&out_ch_layout);
        swr_free(&st.swrCtx);
        avcodec_free_context(&st.vCtx);
        avcodec_free_context(&st.aCtx);
        avformat_close_input(&st.fmtCtx);
        return -1;
    }

    st.swsCtx = sws_getContext(video_w,video_h,AV_PIX_FMT_NV12,video_w,video_h,AV_PIX_FMT_YUV420P,SWS_BICUBIC,nullptr,nullptr,nullptr);
    if(!st.swsCtx){
        std::cerr << "Cannot init resampler" << std::endl;
        swr_free(&st.swrCtx);
        avcodec_free_context(&st.vCtx);
        avcodec_free_context(&st.aCtx);
        avformat_close_input(&st.fmtCtx);
        return -1;
    }

    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0){
        std::cerr << "SDL init failed: " << SDL_GetError() << std::endl;
        swr_free(&st.swrCtx);
        avcodec_free_context(&st.vCtx);
        avcodec_free_context(&st.aCtx);
        avformat_close_input(&st.fmtCtx);
        return -1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);

    SDL_Window *win = SDL_CreateWindow("player", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        window_w, window_h, SDL_WINDOW_SHOWN | SDL_WINDOW_OPENGL);


    if(!win){
        std::cerr << "Cannot create window: " << SDL_GetError() << std::endl;
        SDL_Quit();
        swr_free(&st.swrCtx);
        avcodec_free_context(&st.vCtx);
        avcodec_free_context(&st.aCtx);
        avformat_close_input(&st.fmtCtx);
        return -1;
    }

    SDL_GLContext glCtx = SDL_GL_CreateContext(win);
    if(!glCtx){
        std::cerr << "Cannot create OpenGL Context : " << SDL_GetError() << std::endl;
        SDL_DestroyWindow(win);
        SDL_Quit();
        swr_free(&st.swrCtx);
        avcodec_free_context(&st.vCtx);
        avcodec_free_context(&st.aCtx);
        avformat_close_input(&st.fmtCtx);
        return -1;
    }

    if(!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)){
        std::cerr << "GLAD init failed Unable to load OpenGL function pointer" << std::endl;
        SDL_GL_DeleteContext(glCtx);
        SDL_DestroyWindow(win);
        SDL_Quit();
        exit(-1);
    }

  

    unsigned int texY, texUV;
    glGenTextures(1,&texY);
    glBindTexture(GL_TEXTURE_2D,texY);

    glTexImage2D(GL_TEXTURE_2D,0,GL_R8,video_w,video_h,0,GL_RED,GL_UNSIGNED_BYTE,nullptr);


    glGenTextures(1,&texUV);
    glBindTexture(GL_TEXTURE_2D,texUV);

    glTexImage2D(GL_TEXTURE_2D,0,GL_RG8,video_w / 2, video_h / 2, 0, GL_RG, GL_UNSIGNED_BYTE,nullptr);

    cudaGraphicsResource *cudaTexY = nullptr;
    cudaGraphicsResource *cudaTexUV = nullptr;

    cudaError_t ret;


    ret = cudaGraphicsGLRegisterImage(
        &cudaTexY,
        texY,
        GL_TEXTURE_2D,
        cudaGraphicsRegisterFlagsWriteDiscard
    );


    if(ret != cudaSuccess)
    {
        std::cout 
            << "register texY failed: "
            << cudaGetErrorString(ret)
            << std::endl;
    }



    ret = cudaGraphicsGLRegisterImage(
        &cudaTexUV,
        texUV,
        GL_TEXTURE_2D,
        cudaGraphicsRegisterFlagsWriteDiscard
    );


    if(ret != cudaSuccess)
    {
        std::cout 
            << "register texUV failed: "
            << cudaGetErrorString(ret)
            << std::endl;
    }


    float vertices[] = {
        // position      // texcoord
        -1.0f,  1.0f,     0.0f, 0.0f,
        -1.0f, -1.0f,     0.0f, 1.0f,
         1.0f, -1.0f,     1.0f, 1.0f,
         1.0f,  1.0f,     1.0f, 0.0f
    };

    unsigned int indices[] = {
        0,1,2,
        0,2,3
    };

    unsigned int VAO,VBO,EBO;


    glGenVertexArrays(1,&VAO);
    glGenBuffers(1,&VBO);
    glGenBuffers(1,&EBO);


    glBindVertexArray(VAO);


    glBindBuffer(
        GL_ARRAY_BUFFER,
        VBO
    );

    glBufferData(
        GL_ARRAY_BUFFER,
        sizeof(vertices),
        vertices,
        GL_STATIC_DRAW
    );



    glBindBuffer(
        GL_ELEMENT_ARRAY_BUFFER,
        EBO
    );

    glBufferData(
        GL_ELEMENT_ARRAY_BUFFER,
        sizeof(indices),
        indices,
        GL_STATIC_DRAW
    );

    glVertexAttribPointer(
        0,
        2,
        GL_FLOAT,
        GL_FALSE,
        4*sizeof(float),
        (void*)0
    );

    glEnableVertexAttribArray(0);

    glVertexAttribPointer(
        1,
        2,
        GL_FLOAT,
        GL_FALSE,
        4*sizeof(float),
        (void*)(2*sizeof(float))
    );

    glEnableVertexAttribArray(1);

    //Shader shader("vertex.glsl","fragment.glsl");
    
    Shader shader(
        "shaders/vertex.glsl",
        "shaders/fragment.glsl"
    );

    // SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    // SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, video_w, video_h);

    SDL_AudioSpec want = {};
    want.freq = st.aCtx->sample_rate;
    want.format = AUDIO_F32LSB;
    want.channels = 2;
    want.samples = SDL_samples;
    want.callback = audio_callback;
    want.userdata = (void*)&st;

    //st.audioDev = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);

    if(SDL_OpenAudio(&want,nullptr) < 0){
        std::cerr << "Audio Device open failed" << std::endl;
        SDL_Quit();
        swr_free(&st.swrCtx);
        avcodec_free_context(&st.vCtx);
        avcodec_free_context(&st.aCtx);
        avformat_close_input(&st.fmtCtx);
        return -1;
    }


    double avg_framerate = av_q2d(st.fmtCtx->streams[st.vIdx]->avg_frame_rate);
    if(avg_framerate <= 0.0){
        avg_framerate = av_q2d(st.fmtCtx->streams[st.vIdx]->r_frame_rate);
    }
    if(avg_framerate <= 0.0){
        avg_framerate = 30.0;
    }

    int drawable_w, drawable_h;

    SDL_GL_GetDrawableSize(win, &drawable_w, &drawable_h);

    glViewport(0, 0, drawable_w, drawable_h);

    std::thread tRead(readThread, &st);
    std::thread tVideo(videoThread, &st);
    std::thread tAudio(audioThread, &st);

    //SDL_PauseAudioDevice(st.audioDev, 0);
    SDL_PauseAudio(0);

    double frame_last_pts = 0.0;

    double now_clock;

    bool paused = false;
    double pause_start;
    double frame_timer = SDL_GetTicks();
    while(!quit){
        while(SDL_PollEvent(&event)){
            HandleEvent(&st,event);
        }

        double pts = 0;

        if(Status == PAUSE){
            if(!paused){
                pause_start = SDL_GetTicks();
                paused=true;
            }
            SDL_Delay(4);
            continue;
        }
        
        if(paused){
            frame_timer += SDL_GetTicks() - pause_start;
            paused = false;
        }



        AVFrame *frame = st.vfq.try_pop();
        if(!frame){
            SDL_Delay(3);
            continue;
        }

        AVHWFramesContext* hwctx = nullptr;

        if (frame->hw_frames_ctx == nullptr) {
            std::cerr << "frame->hw_frames_ctx is null\n";
            av_frame_free(&frame);
            continue;
        }

        hwctx = reinterpret_cast<AVHWFramesContext*>(
            frame->hw_frames_ctx->data);

        if (frame->format != AV_PIX_FMT_CUDA ||
            hwctx->sw_format != AV_PIX_FMT_NV12) {
            std::cerr << "Unexpected frame format\n";
            av_frame_free(&frame);
            continue;
        }

        // 将 CUDA 硬件帧传输为普通内存中的 NV12 帧
        AVFrame* nv12_frame = av_frame_alloc();

        if (!nv12_frame) {
            std::cerr << "av_frame_alloc failed\n";
            av_frame_free(&frame);
            continue;
        }

        nv12_frame->format = AV_PIX_FMT_NV12;
        nv12_frame->width = frame->width;
        nv12_frame->height = frame->height;

        int ret = av_hwframe_transfer_data(
            nv12_frame,
            frame,
            0
        );

        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE] = {};
            av_strerror(ret, errbuf, sizeof(errbuf));

            std::cerr << "av_hwframe_transfer_data failed: "
                << errbuf << '\n';

            av_frame_free(&nv12_frame);
            av_frame_free(&frame);
            continue;
        }

        if (nv12_frame->format != AV_PIX_FMT_NV12 ||
            !nv12_frame->data[0] ||
            !nv12_frame->data[1]) {
            std::cerr << "Invalid NV12 frame data\n";

            av_frame_free(&nv12_frame);
            av_frame_free(&frame);
            continue;
        }

        // 注意：这里取得的是 CPU 内存指针，不是 CUDA 设备指针
        uint8_t* srcY = nv12_frame->data[0];
        uint8_t* srcUV = nv12_frame->data[1];

        int srcPitchY = nv12_frame->linesize[0];
        int srcPitchUV = nv12_frame->linesize[1];

        std::cout << "NV12 Y pitch: " << srcPitchY << '\n';
        std::cout << "NV12 UV pitch: " << srcPitchUV << '\n';

        cudaGraphicsMapResources(
            1,
            &cudaTexY,
            0
        );

        cudaArray_t arrayY;


        cudaGraphicsSubResourceGetMappedArray(
            &arrayY,
            cudaTexY,
            0,
            0
        );

        cudaGraphicsMapResources(
            1,
            &cudaTexUV,
            0
        );


        cudaArray_t arrayUV;


        cudaGraphicsSubResourceGetMappedArray(
            &arrayUV,
            cudaTexUV,
            0,
            0
        );

        cudaError_t err = cudaMemcpy2DToArray(
            arrayY, 0, 0,
            srcY, srcPitchY,
            video_w, video_h,
            cudaMemcpyHostToDevice
        );

        if (err != cudaSuccess) {
            std::cerr << "Copy Y failed: "
                << cudaGetErrorString(err) << '\n';
        }

        err = cudaMemcpy2DToArray(
            arrayUV, 0, 0,
            srcUV, srcPitchUV,
            video_w, video_h / 2,
            cudaMemcpyHostToDevice
        );

        if (err != cudaSuccess) {
            std::cerr << "Copy UV failed: "
                << cudaGetErrorString(err) << '\n';
        }

        cudaGraphicsUnmapResources(
            1,
            &cudaTexY,
            0
        );


        cudaGraphicsUnmapResources(
            1,
            &cudaTexUV,
            0
        );

        shader.use();

        // 绑定Y
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D,texY);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        // 绑定UV
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D,texUV);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        // 告诉shader
        glUniform1i(
            glGetUniformLocation(
                shader.GetShaderPragram(),
                "texY"),
                0
        );

        glUniform1i(
            glGetUniformLocation(
            shader.GetShaderPragram(),
            "texUV"),
            1
        );

        // std::cout << av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format)) << std::endl;

        if(frame->pts != AV_NOPTS_VALUE){
            pts = frame->pts * av_q2d(st.vTimeBase);
            if(seek_done){
                frame_last_pts = NAN;
                //st.audio_clock = pts;
                frame_timer = SDL_GetTicks();
                seek_done = false;
            }

        }else if(!std::isnan(frame_last_pts)){
            pts = frame_last_pts + 1.0 / avg_framerate;
        }else{
            pts = getAudioClock(&st);
        }

        

        double delay = 1.0 / avg_framerate;
        if(!std::isnan(frame_last_pts) && pts > frame_last_pts){
            delay = pts - frame_last_pts;
        }
        if(delay <= 0 || delay > 1.0){
            delay = 1.0 / avg_framerate;
        }

        now_clock = SDL_GetTicks();
        if((now_clock - last_sync_check_ticks) > SYNC_CHECK_INTERVAL){
            double diff = pts - getAudioClock(&st);
            if(fabs(diff) > BIG_DIFF_TIMESOUT || std::isnan(diff)){
                if(diff < -BIG_DIFF_TIMESOUT || std::isnan(diff)){
                    av_frame_free(&frame);
                    frame_last_pts = NAN;
                    while(SDL_PollEvent(&event)){
                        HandleEvent(&st,event);
                    }
                    continue;
                }
            }
            else{
                audio_sync_offset = audio_sync_offset + diff * 0.08;
                if(audio_sync_offset > MAX_STEP_ADJUST) audio_sync_offset = MAX_STEP_ADJUST;
                if(audio_sync_offset < -MAX_STEP_ADJUST) audio_sync_offset = -MAX_STEP_ADJUST;
            }
            last_sync_check_ticks = now_clock;
        }

        delay += audio_sync_offset;

        if(delay < 0.005) delay = 0.005;

        Uint32 delay_ms = (Uint32)(delay * 1000);
        Uint32 target = frame_timer + delay_ms;

        if(target > SDL_GetTicks()){
            while(SDL_GetTicks() < target && !quit){
                while(SDL_PollEvent(&event)){
                    HandleEvent(&st,event);
                }
                SDL_Delay(1);
            }
        }

        //std::cout << av_get_pix_fmt_name((AVPixelFormat)frame->format) << std::endl;

        // SDL_UpdateYUVTexture(tex, nullptr,
        //     frame->data[0], frame->linesize[0],
        //     frame->data[1], frame->linesize[1],
        //     frame->data[2], frame->linesize[2]);
        // SDL_RenderClear(ren);
        // SDL_RenderCopy(ren, tex, nullptr, &dstrect);
        // SDL_RenderPresent(ren);

        glBindVertexArray(VAO);

        glDrawElements(
            GL_TRIANGLES,
            6,
            GL_UNSIGNED_INT,
            0
        );


        SDL_GL_SwapWindow(win);

        frame_last_pts = pts;
        frame_timer = target;

        av_frame_free(&frame);
        av_frame_free(&nv12_frame);
    }

    g_running = false;


    tRead.join();
    tVideo.join();
    tAudio.join();

    st.vpq.flush();
    st.apq.flush();

    av_buffer_unref(&hw_device_ctx);

    SDL_CloseAudio();
    //SDL_CloseAudioDevice(st.audioDev);
    // SDL_DestroyTexture(tex);
    // SDL_DestroyRenderer(ren);

    glDeleteBuffers(1,&VBO);
    glDeleteVertexArrays(1,&VAO);
    glDeleteBuffers(1,&EBO); 
    

    SDL_DestroyWindow(win);
    SDL_Quit();

    av_channel_layout_uninit(&out_ch_layout);
    swr_free(&st.swrCtx);
    sws_freeContext(st.swsCtx);
    avcodec_free_context(&st.vCtx);
    avcodec_free_context(&st.aCtx);
    avformat_close_input(&st.fmtCtx);

    return 0;
}
