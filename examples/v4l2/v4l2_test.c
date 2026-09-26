#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

// 如果系统调用恰好被信号中断并返回 EINTR，就重新执行，而不是误报设备故障。
static int xioctl(int fd, unsigned long request, void *arg)
{
    int rc;

    do {
        rc = ioctl(fd, request, arg);
    } while (rc == -1 && errno == EINTR);

    return rc;
}

static int camera_open_checked(const char *device_path)
{
    struct v4l2_capability cap = {0};
    uint32_t caps;
    int fd;

    // O_RDWR: v4l2驱动要求设备以读写方式打开才能配置
    // O_CLOEXEC: 如果程序以后启动另一个程序，斌面摄像头文件描述符泄漏给新进程
    fd = open(device_path, O_RDWR | O_CLOEXEC);
    if (fd == -1) {
        fprintf(stderr, "open %s failed: %s\n",
                device_path, strerror(errno));
        return -1;
    }
    // VIDIOC_QUERYCAP 它向驱动询问节点能力，不会启动摄像头，驱动把结果写入cap
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) == -1) {
        fprintf(stderr, "VIDIOC_QUERYCAP failed: %s\n",
                strerror(errno));
        close(fd);
        return -1;
    }
    // cap.capabilities 描述的是整个物理设备的综合能力，
    // 当前节点自己的能力要从 cap.device_caps 读取
    if (cap.capabilities & V4L2_CAP_DEVICE_CAPS) {
        caps = cap.device_caps;
    } else {
        caps = cap.capabilities;
    }

    if (!(caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE)) {
        fprintf(stderr, "%s does not support multiplanar capture\n",
                device_path);
        close(fd);
        return -1;
    }

    if (!(caps & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "%s does not support streaming\n",
                device_path);
        close(fd);
        return -1;
    }

    printf("driver=%s card=%s bus=%s\n",
           cap.driver, cap.card, cap.bus_info);
    printf("camera capability check passed\n");

    return fd;
}

static int camera_negotiate_nv12(int fd,
                                 uint32_t requested_width,
                                 uint32_t requested_height)
{
    struct v4l2_format fmt = {0};

    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;//多平面类型
    fmt.fmt.pix_mp.width = requested_width;
    fmt.fmt.pix_mp.height = requested_height;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix_mp.field = V4L2_FIELD_ANY;

    // --set-fmt-video
    //         ↓
    // VIDIOC_S_FMT
    //         ↓
    // struct v4l2_format
    if(xioctl(fd, VIDIOC_S_FMT, &fmt) == -1)
    {
        fprintf(stderr, "VIDIOC_S_FMT failed: %s\n", strerror(errno));
        return -1;
    }

    if (fmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12)
    {
        return -1;
    }
    
    if (fmt.fmt.pix_mp.num_planes < 1)
    {
        return -1;
    }
    printf("actual format: width=%u height=%u "
           "fourcc=%c%c%c%c planes=%u stride=%u sizeimage=%u\n",
           (unsigned int)fmt.fmt.pix_mp.width,
           (unsigned int)fmt.fmt.pix_mp.height,
           (int)(fmt.fmt.pix_mp.pixelformat & 0xffU),
           (int)((fmt.fmt.pix_mp.pixelformat >> 8) & 0xffU),
           (int)((fmt.fmt.pix_mp.pixelformat >> 16) & 0xffU),
           (int)((fmt.fmt.pix_mp.pixelformat >> 24) & 0xffU),
           (unsigned int)fmt.fmt.pix_mp.num_planes,
           (unsigned int)fmt.fmt.pix_mp.plane_fmt[0].bytesperline,
           (unsigned int)fmt.fmt.pix_mp.plane_fmt[0].sizeimage);
    return 0;
}

/*
获取一帧照片
v4l2-ctl -d /dev/video-camera0 
        --set-fmt-video=width=1280,height=720,pixelformat=NV12 
        --stream-mmap=4 
        --stream-skip=20 
        --stream-count=1 
        --stream-to=/tmp/camera-1280*720.nv12
v4l2标准流程：
    open()
        ↓
    VIDIOC_QUERYCAP
        ↓
    VIDIOC_S_FMT
        ↓
    VIDIOC_S_PARM
        ↓
    VIDIOC_REQBUFS
        ↓
    VIDIOC_QUERYBUF
        ↓
    mmap()
        ↓
    VIDIOC_QBUF
        ↓
    VIDIOC_STREAMON
        ↓
    循环：
        VIDIOC_DQBUF
        fwrite()
        VIDIOC_QBUF
        ↓
    VIDIOC_STREAMOFF
        ↓
    munmap()
        ↓
    close()
*/
int main(void)
{
    int camera_fd = camera_open_checked("/dev/video-camera0");
    if (camera_fd < 0)
    {
        fprintf(stderr, "camera open failed! \n");
        return 1;
    }

    if (camera_negotiate_nv12(camera_fd, 1280, 720) != 0) {
        close(camera_fd);
        return 1;
    }


    close(camera_fd);
    return 0;
}