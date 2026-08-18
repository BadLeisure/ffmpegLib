package com.example.ffmpegndk;


import androidx.annotation.NonNull;

import java.nio.ByteBuffer;

// Java 层回调接口
public interface FFmpegCallback {
    void onError(int type);
    void onProgress(int percent); // 可选
    void onComplete();
    void onI420Data(ByteBuffer buffer, int width, int height,double pts);
    void onSEIData(byte[] seiData);
}