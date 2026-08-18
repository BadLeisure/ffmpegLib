package com.example.ffmpegndk;

import androidx.annotation.NonNull;
import androidx.appcompat.app.AppCompatActivity;

import android.content.Context;
import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.widget.Button;

import com.example.ffmpegndk.databinding.ActivityMainBinding;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.ByteBuffer;

public class MainActivity extends AppCompatActivity {

    private static final String TAG = "MainActivity";

    public static String RTSP_URL_TWO = "rtsp://172.16.112.135/live/test";


    private Button buttonStartRtsp;
    private Button buttonStopRtsp;

    // Used to load the 'ffmpegndk' library on application startup.
    static {
        System.loadLibrary("ffmpeg_for_android");
    }

    private ActivityMainBinding binding;
    String path;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        binding = ActivityMainBinding.inflate(getLayoutInflater());
        setContentView(binding.getRoot());

        // Example of a call to a native method
        buttonStartRtsp = binding.startRtsp;
        buttonStopRtsp = binding.stopRtsp;

        initLinkJava();

    }

    public void initLinkJava(){
        initLink(frameCallback);
        buttonStopRtsp.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                destroy();
            }
        });
        buttonStartRtsp.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                startRtsp(RTSP_URL_TWO);
            }
        });


    }


    @Override
    protected void onDestroy() {
        super.onDestroy();
        Log.d(TAG, "onDestroy: ");

    }

    /**
     * A native method that is implemented by the 'ffmpegndk' native library,
     * which is packaged with this application.
     */

    public native void initLink(FFmpegCallback frameCallback);

    public native void startRtsp(String rtspUrl);

    public native void destroy();

    FFmpegCallback frameCallback = new FFmpegCallback() {
        @Override
        public void onError(int type) {
            Log.d(TAG, "onError msg："+type);
        }

        @Override
        public void onProgress(int percent) {

        }

        @Override
        public void onComplete() {
            Log.d(TAG, "onComplete: ");
        }

        @Override
        // 注意 ByteBuffer 一定是 direct buffer
        public void onI420Data(ByteBuffer buffer, int width, int height,double pts) {
            Log.d(TAG, "onJavaByteBuffer width: "+width + ",height:"+height+",buffer.capacity():"+buffer.capacity()+",pts:"+pts);
            // 容量检查
            int ySize = width * height;
            int uvSize = ySize / 4;
            int totalSize = ySize + uvSize + uvSize;
            if (buffer == null || buffer.capacity() < totalSize) {
                Log.e(TAG, "Invalid ByteBuffer");
                return;
            }

            // 将 ByteBuffer 转为 byte[]，也可以直接用 ByteBuffer 渲染
            byte[] yuv = new byte[totalSize];
//            buffer.get(yuv); // 注意：get 会移动 position，如果想多次读取需要 buffer.duplicate()

//            // 分割 YUV
//            byte[] y = new byte[ySize];
//            byte[] u = new byte[uvSize];
//            byte[] v = new byte[uvSize];
//
//            System.arraycopy(yuv, 0, y, 0, ySize);
//            System.arraycopy(yuv, ySize, u, 0, uvSize);
//            System.arraycopy(yuv, ySize + uvSize, v, 0, uvSize);

            // 这里可以交给 OpenGL 渲染，或者 SurfaceView 渲染
//            renderYUV420P(y, u, v, width, height);
        }

        @Override
        public void onSEIData(byte[] seiData) {
            Log.d(TAG, "onSeiData: ");
            StringBuilder hex = new StringBuilder();
            for (byte b : seiData) {
                hex.append(String.format("%02X ", b));
            }
            Log.d(TAG, "SEI Data (hex): " + hex.toString());
        }

    };
}