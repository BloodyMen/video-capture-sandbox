#include "CaptureDevice.hpp"

#include <cassert>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <iomanip>
#include <iostream>
#include <libv4l2.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace std;


static int xioctl(int fileDescriptor, int request, void *arg);


CaptureDevice::CaptureDevice() :
        m_fileDescriptor(-1),
        m_captureThread(0)
{
    // cerr << __PRETTY_FUNCTION__ << endl;
}


CaptureDevice::~CaptureDevice()
{
    // cerr << __PRETTY_FUNCTION__ << endl;
    assert(m_fileDescriptor == -1);
}


const string& CaptureDevice::fileName() const
{
    return m_deviceFileName;
}
pair<unsigned int, unsigned int> CaptureDevice::captureSize() const
{
    return make_pair(m_captureWidth, m_captureHeight);
}
__u32 CaptureDevice::pixelFormat() const
{
    return m_pixelFormat;
}
string CaptureDevice::pixelFormatString() const
{
    string ret;
    ret.push_back((char) ((m_pixelFormat & 0x000000ff) >> 0));
    ret.push_back((char) ((m_pixelFormat & 0x0000ff00) >> 8));
    ret.push_back((char) ((m_pixelFormat & 0x00ff0000) >> 16));
    ret.push_back((char) ((m_pixelFormat & 0xff000000) >> 24));
    return  ret;
}
enum v4l2_field CaptureDevice::fieldFormat() const
{
    return m_fieldFormat;
}
unsigned int CaptureDevice::bufferSize() const
{
    return m_bufferSize;
}
unsigned int CaptureDevice::readTimeOut() const
{
    return m_readTimeOut;
}
clockid_t CaptureDevice::clockId() const
{
    return m_timerClockId;
}


bool CaptureDevice::init(const string& deviceFileName, __u32 pixelFormat, unsigned int captureWidth,
        unsigned int captureHeight, unsigned int buffersCount, clockid_t clockId,
        unsigned int readTimeOut )
{
    // cerr << __PRETTY_FUNCTION__ << endl;
    assert(m_fileDescriptor == -1);
    assert(buffersCount > 1);


    m_captureHeight = captureHeight;
    m_captureWidth = captureWidth;
    m_deviceFileName = deviceFileName;
    m_fieldFormat = V4L2_FIELD_NONE;
    m_pixelFormat = pixelFormat;
    m_readTimeOut = readTimeOut;
    m_timerClockId = clockId;
    m_captureThreadCancellationFlag = false;


    /* *** initialize timer *** */
    int clockret = clock_gettime(m_timerClockId, &m_timerStart);
    clock_getres(m_timerClockId, &m_timerResolution);
    gettimeofday(&m_realStartTime, 0);

    if (clockret == -1) {
        if (errno == EINVAL) {
            cerr << __PRETTY_FUNCTION__ << "Choosen clock is not available. abort" << endl;
        } else {
            /* unexpected */
            assert(0);
        }
        finish(); return false;
    }


    /* *** open the device file *** */
    struct stat st;

    if (stat(m_deviceFileName.c_str(), &st) == -1) {
        cerr << __PRETTY_FUNCTION__ << " Cannot identify file. " << errno << strerror(errno) << endl;
        finish(); return false;
    }

    if (!S_ISCHR (st.st_mode)) {
        cerr << "File is no device."  << endl;
        finish(); return false;
    }

    m_fileDescriptor = v4l2_open(m_deviceFileName.c_str(), O_RDWR|O_NONBLOCK);

    if (m_fileDescriptor == -1) {
        cerr << "Cannot open file. " << errno << strerror (errno) << endl;
        finish(); return false;
    }

   
    /* *** initialize capturing *** */
    struct v4l2_capability cap;

    if (xioctl(m_fileDescriptor, VIDIOC_QUERYCAP, &cap) == -1) {
        if (EINVAL == errno) {
            cerr << "File is no V4L2 device." << endl;
            finish(); return false;
        } else {
            cerr << __PRETTY_FUNCTION__ << " VIDIOC_QUERYCAP " << errno << strerror(errno) << endl;
            finish(); return false;
        }
    }

    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        cerr << "File is no video capture device." << endl;
        finish(); return false;
    }

    if (!(cap.capabilities & V4L2_CAP_READWRITE)) {
        cerr << "File does not support read i/o." << endl;
        finish(); return false;
    }


    struct v4l2_cropcap cropcap;
    struct v4l2_crop crop;
    memset(&cropcap, 0, sizeof(v4l2_cropcap));

    cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    xioctl(m_fileDescriptor, VIDIOC_CROPCAP, &cropcap);

    crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    crop.c = cropcap.defrect;
    xioctl(m_fileDescriptor, VIDIOC_S_CROP, &crop); /* ignore errors */


    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(v4l2_format));

    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = m_captureWidth;
    fmt.fmt.pix.height = m_captureHeight;
    fmt.fmt.pix.pixelformat = m_pixelFormat;
    fmt.fmt.pix.field = m_fieldFormat;

    if (xioctl(m_fileDescriptor, VIDIOC_S_FMT, &fmt) == -1) {
        cerr << __PRETTY_FUNCTION__ << " VIDIOC_S_FMT " << errno << strerror(errno) << endl;
        finish(); return false;
    }

    if (fmt.fmt.pix.width != m_captureWidth || fmt.fmt.pix.height != m_captureHeight ||
            fmt.fmt.pix.pixelformat != m_pixelFormat ||
            fmt.fmt.pix.field != m_fieldFormat) {

        cerr << "Your parameters were changed: "
                << m_captureWidth << "x" << m_captureHeight << " in "
                << pixelFormatString() << ", fieldFormat " << m_fieldFormat << " -> ";

        m_captureWidth = fmt.fmt.pix.width;
        m_captureHeight = fmt.fmt.pix.height;
        m_pixelFormat = fmt.fmt.pix.pixelformat;
        m_fieldFormat = fmt.fmt.pix.field;

        cerr << m_captureWidth << "x" << m_captureHeight << " in "
                << pixelFormatString() << ", fieldFormat " << m_fieldFormat << endl;
    }


    /* Buggy driver paranoia. */
    unsigned int min;
    min = fmt.fmt.pix.width * 2;
    if (fmt.fmt.pix.bytesperline < min)
        fmt.fmt.pix.bytesperline = min;
    min = fmt.fmt.pix.bytesperline * fmt.fmt.pix.height;
    if (fmt.fmt.pix.sizeimage < min)
        fmt.fmt.pix.sizeimage = min;

    m_bufferSize = fmt.fmt.pix.sizeimage;

    /* *** allocate buffers *** */
    for (unsigned int a=0; a < buffersCount; ++a) {
        m_buffers.push_front(Buffer());

        m_buffers.front().time = {numeric_limits<time_t>::min(), 0};
        m_buffers.front().readerCount = 0;
        m_buffers.front().buffer = (unsigned char*) malloc(sizeof(unsigned char)*m_bufferSize); 
        assert(m_buffers.front().buffer != 0);

        m_timelySortedBuffers.push_back(&(m_buffers.front()));
    }

    return true;
}


void CaptureDevice::finish()
{
    /* *** stop capturing *** */
    if (m_captureThread != 0) {
        stopCapturing();
    }


    /* *** close device *** */
    if (m_fileDescriptor != -1) {
        if (v4l2_close(m_fileDescriptor) == -1) {
            cerr << __PRETTY_FUNCTION__ << "Could not close device file. " << errno << " " << strerror(errno) << endl;
        }
        m_fileDescriptor = -1;
    }


    /* *** free buffers *** */
    if (m_buffers.empty() == false) {
        for (auto a = m_buffers.begin(); a != m_buffers.end(); ++a) {
            assert(a->buffer != 0);
            free (a->buffer); a->buffer = 0;
        }
        m_buffers.clear();
    }
    m_bufferSize = 0;
}


void CaptureDevice::printDeviceInfo() const
{
    // cerr << __PRETTY_FUNCTION__ << endl;
    assert(m_fileDescriptor != -1);

	struct v4l2_capability cap;
	/* check capabilities */
	if (xioctl(m_fileDescriptor, VIDIOC_QUERYCAP, &cap) == -1) {
        if (EINVAL == errno) {
            cerr << "Device is no V4L2 device." << endl;
            assert(0);
        } else {
            cerr << __PRETTY_FUNCTION__ << " VIDIOC_QUERYCAP " << errno << strerror(errno) << endl;
        }
	}

    cout << "Device info:" << endl
            << "  driver: " << cap.driver << endl
            << "  card: " << cap.card << endl
            << "  bus info: " << cap.bus_info << endl
            << "  version: " << cap.version << endl;

    cout << "  supports: ";

	if (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) {
        cout << "capture, ";
    }

	if (cap.capabilities & V4L2_CAP_STREAMING) {
        cout << "streaming";
    }
    cout << endl;
}


void CaptureDevice::printControls() const
{
    // cerr << __PRETTY_FUNCTION__ << endl;
    assert(m_fileDescriptor != -1);

    struct v4l2_queryctrl ctrl;
    memset (&ctrl, 0, sizeof(v4l2_queryctrl ));

    cout << "Available Controls:" << endl;
    for (__u32 id = V4L2_CID_BASE; id < V4L2_CID_LASTP1; id++) {
        queryControl(id);
    }

    cout << "Available Private Controls:" << endl;
    for (__u32 id = V4L2_CID_PRIVATE_BASE;; ++id) {
        /* an invalid id means we are beyond fence */
        if (queryControl(id) == true) break;
    }
}


void CaptureDevice::printFormats() const
{
    // cerr << __PRETTY_FUNCTION__ << endl;
    assert(m_fileDescriptor != -1);

    struct FormatRecord
    {
        unsigned int id;
        const char *name;
    };

    /* copied from the videodev2.h header */
    FormatRecord pixelFormats[] = {
            { V4L2_PIX_FMT_RGB332, "V4L2_PIX_FMT_RGB332" },
            { V4L2_PIX_FMT_RGB444, "V4L2_PIX_FMT_RGB444" },
            { V4L2_PIX_FMT_RGB555, "V4L2_PIX_FMT_RGB555" },
            { V4L2_PIX_FMT_RGB565, "V4L2_PIX_FMT_RGB565" },
            { V4L2_PIX_FMT_RGB555X, "V4L2_PIX_FMT_RGB555X" },
            { V4L2_PIX_FMT_RGB565X, "V4L2_PIX_FMT_RGB565X" },
            { V4L2_PIX_FMT_BGR24, "V4L2_PIX_FMT_BGR24" },
            { V4L2_PIX_FMT_RGB24, "V4L2_PIX_FMT_RGB24" },
            { V4L2_PIX_FMT_BGR32, "V4L2_PIX_FMT_BGR32" },
            { V4L2_PIX_FMT_RGB32, "V4L2_PIX_FMT_RGB32" },
            { V4L2_PIX_FMT_GREY, "V4L2_PIX_FMT_GREY" },
            { V4L2_PIX_FMT_Y16, "V4L2_PIX_FMT_Y16" },
            { V4L2_PIX_FMT_PAL8, "V4L2_PIX_FMT_PAL8" },
            { V4L2_PIX_FMT_YVU410, "V4L2_PIX_FMT_YVU410" },
            { V4L2_PIX_FMT_YVU420, "V4L2_PIX_FMT_YVU420" },
            { V4L2_PIX_FMT_YUYV, "V4L2_PIX_FMT_YUYV" },
            { V4L2_PIX_FMT_UYVY, "V4L2_PIX_FMT_UYVY" },
            { V4L2_PIX_FMT_YUV422P, "V4L2_PIX_FMT_YUV422P" },
            { V4L2_PIX_FMT_YUV411P, "V4L2_PIX_FMT_YUV411P" },
            { V4L2_PIX_FMT_Y41P, "V4L2_PIX_FMT_Y41P" },
            { V4L2_PIX_FMT_YUV444, "" },
            { V4L2_PIX_FMT_YUV555, "V4L2_PIX_FMT_YUV555" },
            { V4L2_PIX_FMT_YUV565, "V4L2_PIX_FMT_YUV565" },
            { V4L2_PIX_FMT_YUV32, "V4L2_PIX_FMT_YUV32" },
            { V4L2_PIX_FMT_NV12, "V4L2_PIX_FMT_NV12" },
            { V4L2_PIX_FMT_NV21, "V4L2_PIX_FMT_NV21" },
            { V4L2_PIX_FMT_YUV410, "V4L2_PIX_FMT_YUV410" },
            { V4L2_PIX_FMT_YUV420, "V4L2_PIX_FMT_YUV420" },
            { V4L2_PIX_FMT_YYUV, "V4L2_PIX_FMT_YYUV" },
            { V4L2_PIX_FMT_HI240, "V4L2_PIX_FMT_HI240" },
            { V4L2_PIX_FMT_HM12, "V4L2_PIX_FMT_HM12" },
            { V4L2_PIX_FMT_SBGGR8, "V4L2_PIX_FMT_SBGGR8" },
            { V4L2_PIX_FMT_SGBRG8, "V4L2_PIX_FMT_SGBRG8" },
            { V4L2_PIX_FMT_SGRBG10, "V4L2_PIX_FMT_SGRBG10" },
            { V4L2_PIX_FMT_SGRBG10DPCM8, "V4L2_PIX_FMT_SGRBG10DPCM8" },
            { V4L2_PIX_FMT_SBGGR16, "V4L2_PIX_FMT_SBGGR16" },
            { V4L2_PIX_FMT_MJPEG, "V4L2_PIX_FMT_MJPEG" },
            { V4L2_PIX_FMT_JPEG, "V4L2_PIX_FMT_JPEG" },
            { V4L2_PIX_FMT_DV, "V4L2_PIX_FMT_DV" },
            { V4L2_PIX_FMT_MPEG, "V4L2_PIX_FMT_MPEG" },
            { V4L2_PIX_FMT_WNVA, "V4L2_PIX_FMT_WNVA" },
            { V4L2_PIX_FMT_SN9C10X, "V4L2_PIX_FMT_SN9C10X" },
            { V4L2_PIX_FMT_PWC1, "V4L2_PIX_FMT_PWC1" },
            { V4L2_PIX_FMT_PWC2, "V4L2_PIX_FMT_PWC2" },
            { V4L2_PIX_FMT_ET61X251, "V4L2_PIX_FMT_ET61X251" },
            { V4L2_PIX_FMT_SPCA501, "V4L2_PIX_FMT_SPCA501" },
            { V4L2_PIX_FMT_SPCA505, "V4L2_PIX_FMT_SPCA505" },
            { V4L2_PIX_FMT_SPCA508, "V4L2_PIX_FMT_SPCA508" },
            { V4L2_PIX_FMT_SPCA561, "V4L2_PIX_FMT_SPCA561" },
            { V4L2_PIX_FMT_PAC207, "V4L2_PIX_FMT_PAC207" },
            { V4L2_PIX_FMT_PJPG, "V4L2_PIX_FMT_PJPG" },
            { V4L2_PIX_FMT_YVYU, "V4L2_PIX_FMT_YVYU" }
    };

    int formatCount = sizeof(pixelFormats) / sizeof(FormatRecord);
    

    cout << "Supported Formats: " << endl;

	/* ask for a pixel format enumeration */
	int ioctlError = 0;
	int formatIndex = 0;

	while (ioctlError == 0) {

        struct v4l2_fmtdesc format;
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		format.index = formatIndex;

		ioctlError = ioctl (m_fileDescriptor, VIDIOC_ENUM_FMT, &format);

		if (ioctlError == 0) {
			for (int a = 0; a < formatCount; ++a) {
				if (format.pixelformat == pixelFormats[a].id) {
                    cout << "  " << format.description
                            << ((format.flags & V4L2_FMT_FLAG_COMPRESSED) ? " compressed" : " raw")
                            << " \"" << pixelFormats[a].name << "\"" << endl;
					break;
				}
			}
		}

        ++formatIndex;
	}
}


void CaptureDevice::printTimerInformation() const
{
    struct tm *localTime = localtime(&m_realStartTime.tv_sec);

    cout << "Start Time: "
            << setw(2) << setfill('0') << localTime->tm_year-100
            << setw(2) << setfill('0') << localTime->tm_mon+1
            << setw(2) << setfill('0') << localTime->tm_mday
            << " "
            << setw(2) << setfill('0') << localTime->tm_hour
            << ":"
            << setw(2) << setfill('0') << localTime->tm_min << endl;

    cout << "Timer Resolution: " << (double) m_timerResolution.tv_sec << "s "
            << m_timerResolution.tv_nsec << "nsec" << endl;
}


deque<const CaptureDevice::Buffer*> CaptureDevice::lockFirstNBuffers(unsigned int n)
{
    std::deque<const Buffer*> ret;

    m_timelySortedBuffersMutex.lock();
    unsigned int a = 0;
    for (auto it = m_timelySortedBuffers.begin(); a < n && it != m_timelySortedBuffers.end(); ++it, ++a) {

        ret.push_back(*it);
        ++(*it)->readerCount;

    }
    m_timelySortedBuffersMutex.unlock();


    return ret;
}


void CaptureDevice::unlock(const deque<const Buffer*>& buffers)
{
    m_timelySortedBuffersMutex.lock();
    for (auto it = buffers.begin(); it != buffers.end(); ++it) {
        for (auto it2 = m_timelySortedBuffers.begin(); it2 != m_timelySortedBuffers.end(); ++it2) {

            if (*it == *it2) {
                --((*it2)->readerCount);
                assert((*it2)->readerCount >= 0);
            }
        }
    }
    m_timelySortedBuffersMutex.unlock();
}


unsigned int CaptureDevice::newerBuffersAvailable(const timespec& newerThan)
{
    unsigned int ret = 0;
    auto it = m_timelySortedBuffers.begin();

    m_timelySortedBuffersMutex.lock();

    // cerr << (*it)->time.tv_sec - newerThan.tv_sec << " " << (*it)->time.tv_nsec - newerThan.tv_nsec << endl;

    for (; it != m_timelySortedBuffers.end(); ++it) {
        if (((*it)->time.tv_sec < newerThan.tv_sec) ||
                (((*it)->time.tv_sec == newerThan.tv_sec) && ((*it)->time.tv_nsec <= newerThan.tv_nsec))
                ) {
            break;
        }
        ++ret;

    }
    m_timelySortedBuffersMutex.unlock();

    return ret;
}


pair<double, double> CaptureDevice::determineCapturePeriod(double secondsToIterate)
{
    pair<double, double> ret;

    thread t(bind(determineCapturePeriodThread, secondsToIterate, this, &ret));
    t.join();
    
    return ret;
}


void CaptureDevice::startCapturing()
{
    assert(m_captureThread == 0);
    m_captureThread = new thread(bind(captureThread, this));
}


void CaptureDevice::stopCapturing()
{
    if (m_captureThread != 0) {
        assert(m_captureThread->joinable() == true);

        m_captureThreadCancellationFlag = true;
        m_captureThread->join();
        m_captureThreadCancellationFlag = false;

        delete m_captureThread;
        m_captureThread = 0;
    }
}


bool CaptureDevice::queryControl(__u32 id) const
{
    bool ret = false;
    struct v4l2_queryctrl ctrl;
    memset (&ctrl, 0, sizeof(v4l2_queryctrl ));
    ctrl.id = id;

    if (xioctl(m_fileDescriptor, VIDIOC_QUERYCTRL, &ctrl) == 0) {

        cout << "  " << ctrl.id - V4L2_CID_BASE << " \"" << ctrl.name << "\""
                << ((ctrl.flags & V4L2_CTRL_FLAG_DISABLED) ? " " : " not ") << "disabled,"
                << ((ctrl.flags & V4L2_CTRL_FLAG_GRABBED) ? " " : " not  ") << "grabbed,"
                << ((ctrl.flags & V4L2_CTRL_FLAG_READ_ONLY) ? " " : " not ") << "readonly,"
                << ((ctrl.flags & V4L2_CTRL_FLAG_UPDATE) ? " " : " not ") << "update,"
                << ((ctrl.flags & V4L2_CTRL_FLAG_INACTIVE) ? " " : " not ") << "inactive,"
                << ((ctrl.flags & V4L2_CTRL_FLAG_SLIDER) ? " " : " not ") << "slider,";

        switch (ctrl.type) {
        case V4L2_CTRL_TYPE_INTEGER:
            cout << " integer type";
            break;
        case V4L2_CTRL_TYPE_BOOLEAN:
            cout << " boolean type";
            break;
        case V4L2_CTRL_TYPE_MENU:
            cout << " menu type";
            break;
        case V4L2_CTRL_TYPE_BUTTON:
            cout << " button type";
            break;
        case V4L2_CTRL_TYPE_INTEGER64:
            cout << " integer 64 type";
            break;
        case V4L2_CTRL_TYPE_CTRL_CLASS:
            cout << " control class type";
            break;
        default:
            assert(0);
        }

        cout << endl;

        if (!(ctrl.flags & V4L2_CTRL_FLAG_DISABLED) && ctrl.type == V4L2_CTRL_TYPE_MENU) {

            struct v4l2_querymenu menu;
            memset (&menu, 0, sizeof (v4l2_querymenu));
            menu.id = ctrl.id;

            for (menu.index = ctrl.minimum; ((__s32) menu.index) <= ctrl.maximum; menu.index++) {

                if (xioctl(m_fileDescriptor, VIDIOC_QUERYMENU, &menu) == 0) {
                    cout << "  " << menu.name << endl;
                } else {
                    cerr << __PRETTY_FUNCTION__ << " VIDIOC_QUERYMENU " << errno << strerror(errno) << endl;
                }
            }

        }

    } else if (errno != EINVAL) {
        cerr << __PRETTY_FUNCTION__ << " VIDIOC_QUERYCTRL " << errno << strerror(errno) << endl;
    } else if (errno == EINVAL) {
        ret = true;
    }

    return ret;
}


/* *** static functions ***************************************************** */
void CaptureDevice::determineCapturePeriodThread(double secondsToIterate,
        CaptureDevice* camera, pair<double,double>* ret)
{
    int fileDescriptor = camera->m_fileDescriptor;
    unsigned int bufferSize = camera->m_bufferSize;
    clockid_t clockId = camera->m_timerClockId;
    void *buffer = malloc(camera->m_bufferSize);
    fd_set filedescriptorset;
    struct timeval tv;
    int sel;
    ssize_t readlen;
    vector<struct timespec> times;


    for (;;) {
        times.resize(times.size()+1);
        clock_gettime(clockId, &times.back());

        if (difftime(times.back().tv_sec, times.front().tv_sec) > secondsToIterate) {
            break;
        }

        FD_ZERO(&filedescriptorset);
        FD_SET(fileDescriptor, &filedescriptorset);
        tv.tv_sec = 2;
        tv.tv_usec = 0;

        /* watch the file handle for new readable data */
        sel = select(fileDescriptor + 1, &filedescriptorset, NULL, NULL, &tv);

        if (sel == -1 && errno != EINTR) {
            cerr << __PRETTY_FUNCTION__ << " Select error. " << errno << strerror(errno) << endl;
            abort();
        } else if (sel == 0) {
            cerr << __PRETTY_FUNCTION__ << " Select timeout. " << errno << strerror(errno) << endl;
            assert(0);
        }

        /* read from the device */
        readlen = v4l2_read(fileDescriptor, buffer, bufferSize);

        if (readlen == -1) {
            cerr << __PRETTY_FUNCTION__ << " Read error. " << errno << strerror(errno) << endl;
            assert(0);
        }
    }
    free(buffer);


    /* *** compute interval from timestamps *** */
    vector<double> intervals;
    for (auto a = times.begin(); a != (times.end()-1); ++a) {
        auto b = a+1;
        intervals.push_back(
                (b->tv_sec + b->tv_nsec / 1000000000.0) -
                (a->tv_sec + a->tv_nsec / 1000000000.0));
    }

    /* *** compute period *** */
    double mean = 0.0;
    for (auto a = intervals.begin(); a != intervals.end(); ++a) {
        mean += *a;
    }
    mean /= intervals.size();

    /* *** compute standard deviation **/
    double addedSquares = 0.0;
    for (auto a = intervals.begin(); a != intervals.end(); ++a) {
        addedSquares = ((*a - mean) * (*a - mean));
    }
    double standardDeviation = sqrt(addedSquares / intervals.size());

    ret->first = mean;
    ret->second = standardDeviation;
}


void CaptureDevice::captureThread(CaptureDevice* camera)
{
    int fileDescriptor = camera->m_fileDescriptor;
    unsigned int bufferSize = camera->m_bufferSize;
    clockid_t clockId = camera->m_timerClockId;
    std::deque<Buffer*>& sortedBuffers = camera->m_timelySortedBuffers;
    std::mutex& sortedBuffersMutex =  camera->m_timelySortedBuffersMutex;
    fd_set filedescriptorset;
    struct timeval tv;
    int sel;
    ssize_t readlen;


    while (camera->m_captureThreadCancellationFlag == false) {

#if 1
        FD_ZERO(&filedescriptorset);
        FD_SET(fileDescriptor, &filedescriptorset);
        tv.tv_sec = 2;
        tv.tv_usec = 0;

        /* watch the file handle for new readable data */
        sel = select(fileDescriptor + 1, &filedescriptorset, NULL, NULL, &tv);

        if (sel == -1 && errno != EINTR) {
            cerr << __PRETTY_FUNCTION__ << " Select error. " << errno << strerror(errno) << endl;
            abort();
        } else if (sel == 0) {
            cerr << __PRETTY_FUNCTION__ << " Select timeout. " << errno << strerror(errno) << endl;
            assert(0);
        }
#endif

        /* remove the last element if possible */
        sortedBuffersMutex.lock();

        if (sortedBuffers.back()->readerCount > 0) {
            cerr << "no writeable buffer present. trying hard" << endl;
            continue;
        }
        Buffer* buffer = sortedBuffers.back();
        sortedBuffers.pop_back();

        sortedBuffersMutex.unlock();


        /* read from the device into the buffer */
        clock_gettime(clockId, &(buffer->time));
        readlen = v4l2_read(fileDescriptor, buffer->buffer, bufferSize);

        if (readlen == -1) {
            cerr << __PRETTY_FUNCTION__ << " Read error. " << errno << strerror(errno) << endl;
            assert(0);
        }


        /* insert the newly read buffer as first element - newest picture taken */
        sortedBuffersMutex.lock();
        sortedBuffers.push_front(buffer);
        sortedBuffersMutex.unlock();

        /*cerr << "wrote ";
        for (auto it = sortedBuffers.begin(); it != sortedBuffers.end(); ++it) {
            cerr << (*it)->time.tv_sec << " " << *it << ", ";
        }
        cerr << endl;*/
    }
}


/** helper, which calls ioctl until an undisturbed call has been done */
int xioctl(int fileDescriptor, int request, void *arg)
{
    int r;

    do r = v4l2_ioctl (fileDescriptor, request, arg);
    while (-1 == r && EINTR == errno);

    return r;
}
