#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <sys/mman.h>

int main(void)
{
    int fd = -1;
    // 打开视频设备文件
    fd = open("/dev/vidio0", O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "open error: %s %s \n", "/dev/video0", strerror(errno));
        return -1;
    }   

    struct v4l2_capability vcap;
    // 查询属性，功能
    ioctl(fd, VIDIOC_QUERYCAP, &vcap);

    if (!(V4L2_CAP_VIDEO_CAPTURE & vcap.capabilities))
    {
        fprintf(stderr, "error: no capture video device! \n");
        return -1;
    }

    /* type 字段需要在调用ioctl 之前设置他的值，对于摄像头，
    需要将type字段设置为V4L2_BUF_TYPE_VIDEO_CAPTURE
    指定我们将要获取的是时评采集的像素格式*/
    struct v4l2_fmtdesc fmtdesc;
    fmtdesc.index = 0;
    fmtdesc.type = V4L2_BUF_CAP_VIDEO_CAPTURE;
    // 设置设备参数
    /*枚举出摄像头所支持的所有像素格式以及描述信息*/
    while( 0 == ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc))
    {
        printf("fmt : %s<0x %x>\n",fmtdesc.description, fmtdesc.pixelformat);
        fmtdesc.index++;
    }
    /* 枚举摄像头所支持的素有时评采集分辨率*/
    struct v4l2_frmsizeenum frmsize;

    frmsize.index = 0;
    frmsize.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    frmsize.pixel_format = V4L2_PIX_FMT_RGB565;
    while (0 == ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frmsize))
    {
        printf("frame_size<%d*%d>\n",frmsize.discrete.width, frmsize.discrete.height);
        frmsize.index++;
    }

    /* 枚举摄像头锁支持的所有视频采集帧率 */
    struct v4l2_frmivalenum frmival;

    frmival.index = 0;
    frmival.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    frmival.pixel_format = V4L2_PIX_FMT_RGB565;
    frmival.width = 640;
    frmival.height = 480;

    while (0 == ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &frmival))
    {
        printf("Frame interval<%ffps>",frmival.discrete.denominator / frmival.discrete.numerator);
        frmival.index++;
    }

    /* 查看或设置当前格式 ： VIDIOC_G_FMT VIDIOC_S_FMT */
    struct v4l2_format fmt;
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (0 > ioctl(fd, VIDIOC_G_FMT), &fmt) // 获取格式信息
    {
        perror("ioctl error");
        return -1;
    }
    printf("width:%d, height:%d format:%d\n", fmt.fmt.pix.width, fmt.fmt.pix.height, fmt.fmt.pix.pixelformat);

    fmt.fmt.pix.width = 800;
    fmt.fmt.pix.height = 480;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0)
    {
        perror("ioctl error");
        return -1;
    }

    if (800 != fmt.fmt.pix.width || 480 != fmt.fmt.pix.height)
    {
        printf("width height warning !\n");
    }

    if (V4L2_PIX_FMT_NV12 != fmt.fmt.pix.pixelformat)
    {
        printf("pixelformat warning !\n");
    }

    /* 设置或获取当前流类型相关参数 VIDIOC_G_PARM VIDIOC_S_PARM*/
    struct v4l2_streamparm streamparm;
    streamparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(fd, VIDIOC_G_PARM, &streamparm);

    /* 判断是否支持帧率设置 */
    if (V4L2_CAP_TIMEPERFRAME & streamparm.parm.capture.capability)
    {
        streamparm.parm.capture.timeperframe.numerator = 1;
        streamparm.parm.capture.timeperframe.denominator = 30; // 30fps

        if (ioctl(fd, VIDIOC_S_PARM, &streamparm) < 0)
        {
            fprintf(stderr, "ioctl error: VIDIOC_S_PARM: %s \n", strerror(errno));
            return -1;
        }
    }
    else
    {
        printf("do not support VIDIOC_S_PARM!\n");
    }
    // 申请缓冲区
    /* 将帧缓冲映射到到进程地址空间*/
    struct v4l2_requestbuffers reqbuf;
    struct v4l2_buffer buf;
    void *frm_base[3];

    reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    reqbuf.count = 3; // 申请3个帧缓冲
    reqbuf.memory = V4L2_MEMORY_MMAP;

    if (ioctl(fd, VIDIOC_REQBUFS, &reqbuf) < 0)
    {
        fprintf(stderr,"ioctl error: VIDIOC_REQBUFS %s \n", strerror(errno));
        return -1;
    }
    /*建立内存映射*/
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    for (buf.index = 0; buf.index < 3; buf.index++)
    {
        ioctl(fd, VIDIOC_QUERYBUF, &buf);
        frm_base[buf.index] = mmap(NULL, buf.length, PROT_READ|PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
        if (MAP_FAILED == frm_base[buf.index])
        {
            perror("mmap error");
            return -1;
        }
    }
    // 入队，开始视频采集
    /* 入队操作 */
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    for (buf.index = 0; buf.index < 3; buf.index++)
    {
        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0)
        {
            perror("ioctl error");
            return -1;
        }
        
    }
    // 出队，进行处理
    // ioctl(int fd, VIDIOC_STREAMON, int *type);
    //开启视频采集
    // ioctl(int fd, VIDIOC_STREAMOFF, int *type);
    //停止视频采集
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (0 > ioctl(fd, VIDIOC_STREAMON, &type)) {
        perror("ioctl error");
        return -1;
    }
    // 处理完后，再次入队，往复
    for ( ; ; ) {
        for(buf.index = 0; buf.index < 3; buf.index++) {
            ioctl(fd, VIDIOC_DQBUF, &buf);
            //出队
            // 读取帧缓冲的映射区、获取一帧数据
            // 处理这一帧数据
            // do_something();
            // 数据处理完之后、将当前帧缓冲入队、接着读取下一帧数据
            ioctl(fd, VIDIOC_QBUF, &buf);
        }
    }

    // 结束采集
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (0 > ioctl(fd, VIDIOC_STREAMOFF, &type)) {
        perror("ioctl error");
        return -1;
    }

    close(fd);
    return 0;
}