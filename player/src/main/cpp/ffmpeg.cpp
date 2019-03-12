//
// Created by Administrator on 2019/2/18.
//

#include "ffmpeg.h"

FFmpeg::FFmpeg(FFCallBack *ffCallBack, const char *filePath) {
    this->ffCallBack = ffCallBack;
    this->filePath = filePath;
    pthread_mutex_init(&playMutex, NULL);
    pthread_mutex_init(&seekMutex, NULL);
    this->ffPlayStatus = STATUS_STOP;
}

FFmpeg::~FFmpeg() {
    pthread_mutex_destroy(&playMutex);
}

void *decodeCallBack(void *data) {
    FFmpeg *ffmpeg = (FFmpeg *) data;
    ffmpeg->decodeAudio();
    pthread_exit(&ffmpeg->decodeThread);
}

void FFmpeg::prepare() {
    pthread_create(&decodeThread, NULL, decodeCallBack, this);
}


void FFmpeg::decodeAudio() {
    pthread_mutex_lock(&playMutex);
    //注册所有解码器
    av_register_all();
    //初始化网络连接组件
    avformat_network_init();
    //分配上下文
    avFormatContext = avformat_alloc_context();
    //打开地址成功返回0
    if (avformat_open_input(&avFormatContext, filePath, NULL, NULL) != 0) {
        this->ffCallBack->onErrorCallBack(CALL_CHILD, 1001, "open file failed");
        pthread_mutex_unlock(&playMutex);
        return;
    }

    //找到流媒体成功返回大于等于0
    if (avformat_find_stream_info(avFormatContext, NULL) < 0) {
        this->ffCallBack->onErrorCallBack(CALL_CHILD, 1002, "open stream failed");
        pthread_mutex_unlock(&playMutex);
        return;
    }

    //从流媒体中找音频流并进行存储
    for (int i = 0; i < avFormatContext->nb_streams; i++) {
        if (avFormatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (ffAudio == NULL) {
                this->ffAudio = new FFAudio(this->ffCallBack);
                this->ffAudio->streamIndex = i;
                this->ffAudio->avCodecpar = avFormatContext->streams[i]->codecpar;
                this->ffAudio->allDuration = avFormatContext->duration / AV_TIME_BASE;
                this->ffAudio->avRational = avFormatContext->streams[i]->time_base;
            }
        }
    }
    //根据流属性id找到解码器
    AVCodec *pCodec = avcodec_find_decoder(ffAudio->avCodecpar->codec_id);
    if (!pCodec) {
        this->ffCallBack->onErrorCallBack(CALL_CHILD, 1003, "can not find decoder");
        pthread_mutex_unlock(&playMutex);
        return;
    }
    //通过解码器创建解码器上下文
    this->ffAudio->avCodecContext = avcodec_alloc_context3(pCodec);

    //将解码器中的属性复制到解码器上下文中
    if (avcodec_parameters_to_context(this->ffAudio->avCodecContext, this->ffAudio->avCodecpar) <
        0) {
        this->ffCallBack->onErrorCallBack(CALL_CHILD, 1004,
                                          "can not fill the avCodec to avCodecContext");
        pthread_mutex_unlock(&playMutex);
        return;
    }

    //打开解码器
    if (avcodec_open2(this->ffAudio->avCodecContext, pCodec, 0) < 0) {
        this->ffCallBack->onErrorCallBack(CALL_CHILD, 1005, "can not open avCodec!");
        pthread_mutex_unlock(&playMutex);
        return;
    }

    //prepare成功，通知java层
    if (this->ffCallBack != NULL) {
        this->ffPlayStatus = STATUS_PLAYING;
        this->ffCallBack->onPrepareCallBack(CALL_CHILD);
    } else {
        this->ffPlayStatus = STATUS_STOP;
    }
    if (ffAudio != NULL) {
        this->ffAudio->ffPlayStatus = this->ffPlayStatus;
    }
    pthread_mutex_unlock(&playMutex);
}

//从流中解码出每一帧
void FFmpeg::start() {
    if (this->ffAudio == NULL) {
        this->ffCallBack->onErrorCallBack(CALL_CHILD, 1006, "audio is null");
        return;
    }
    //开启线程，解码AVPacket并重采样生成PCM数据
    this->ffAudio->start();

    //保存AVPacket到队列中
    int count = 0;
    while (this->ffAudio->ffPlayStatus != STATUS_STOP) {
        if (this->ffAudio->ffPlayStatus == STATUS_SEEK ||
            this->ffAudio->ffQueue->getQueueSize() > 40) {
            continue;
        }
        AVPacket *pPacket = av_packet_alloc();
        pthread_mutex_lock(&seekMutex);
        int ret = av_read_frame(this->avFormatContext, pPacket);
        pthread_mutex_unlock(&seekMutex);
        if (ret == 0) {
            if (pPacket->stream_index == this->ffAudio->streamIndex) {
                count++;
                if (LOGDEBUG) {
                    LOGI("解码到第 %d 帧", count);
                }
                this->ffAudio->ffQueue->setFfPlayStatus(this->ffAudio->ffPlayStatus);
                this->ffAudio->ffQueue->pushAVPacket(pPacket);
            } else {
                av_packet_free(&pPacket);
                av_free(pPacket);
                pPacket = NULL;
            }
        } else {
            av_packet_free(&pPacket);
            av_free(pPacket);
            pPacket = NULL;
            //出队完成退出循环
            while (this->ffAudio->ffPlayStatus == STATUS_PLAYING) {
                if (this->ffAudio->ffQueue->getQueueSize() > 0) {
                    continue;
                } else {
                    this->ffAudio->ffPlayStatus = STATUS_STOP;
                }
            }
            break;
        }
    }
    if (this->ffCallBack != NULL) {
        this->ffCallBack->onCompleteCallBack(CALL_CHILD);
    }

}

void FFmpeg::pause() {
    if (this->ffAudio != NULL) {
        this->ffAudio->pause();
    }
}

void FFmpeg::play() {
    if (this->ffAudio != NULL) {
        this->ffAudio->play();
    }
}

void FFmpeg::release() {
    if (this->ffPlayStatus == STATUS_STOP) {
        return;
    } else {
        pthread_mutex_lock(&playMutex);
        this->ffAudio->ffPlayStatus = STATUS_STOP;
        if (this->ffAudio != NULL) {
            this->ffAudio->release();
            delete (this->ffAudio);
            this->ffAudio = NULL;
        }
        if (this->avFormatContext != NULL) {
            avformat_close_input(&avFormatContext);
            avformat_free_context(avFormatContext);
            this->avFormatContext = NULL;
        }
        pthread_mutex_unlock(&playMutex);
    }


}

void FFmpeg::seek(int64_t seconds) {
    if (seconds > 0 && this->ffAudio != NULL && this->ffAudio->allDuration > 0 &&
        seconds < this->ffAudio->allDuration) {
        int tempStatus = this->ffPlayStatus;
        this->ffPlayStatus = STATUS_SEEK;
        this->ffAudio->ffPlayStatus = this->ffPlayStatus;
        this->ffAudio->ffQueue->releaseAVPacket();
        this->ffAudio->currentTime = 0;
        this->ffAudio->newTime = 0;
        pthread_mutex_lock(&seekMutex);
        int64_t seekTime = seconds * AV_TIME_BASE;
        avformat_seek_file(this->avFormatContext, -1, INT64_MIN, seekTime, INT64_MAX, 0);
        pthread_mutex_unlock(&seekMutex);
        this->ffPlayStatus = tempStatus;
        this->ffAudio->ffPlayStatus = this->ffPlayStatus;
    }
}



