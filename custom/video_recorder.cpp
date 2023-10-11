// ----> Includes
#include <iostream>
#include <sstream>
#include <string>

#include <chrono>
#include <thread>

// StereoLabs includes
#include "videocapture.hpp"

// OpenCV includes
#include <opencv2/opencv.hpp>

// Sample includes
#include "calibration.hpp"
#include "stopwatch.hpp"
#include "stereo.hpp"
#include "ocv_display.hpp"
// <---- Includes

#define USE_OCV_TAPI // Comment to use "normal" cv::Mat instead of CV::UMat
#define SCALE_FACTOR 0.25  // Define the scale factor for disparity image writer

int main(int argc, char *argv[])
{
    // ----> Silence unused warning
    (void)argc;
    (void)argv;
    // <---- Silence unused warning

    sl_oc::VERBOSITY verbose = sl_oc::VERBOSITY::INFO;

    int duration = 10;  // recording duration in seconds
    if (argc > 1)
        duration = std::stoi(argv[1]);
    
    std::string recording_name = "recording";
    if (argc > 2)
        recording_name = argv[2];

    // ----> Set Video parameters
    sl_oc::video::VideoParams params;
#ifdef EMBEDDED_ARM
    params.res = sl_oc::video::RESOLUTION::VGA;
#else
    params.res = sl_oc::video::RESOLUTION::HD720;
#endif
    params.verbose = verbose;
    params.fps = sl_oc::video::FPS::FPS_15;
    const int frequency = 15; // 15 Hz
    const std::chrono::milliseconds dt(1000 / frequency);
    const int loop_count = duration * frequency;

    // <---- Set Video parameters

    // ----> Create Video Capture
    sl_oc::video::VideoCapture cap(params);
    if( !cap.initializeVideo(-1) )
    {
        std::cerr << "Cannot open camera video capture" << std::endl;
        std::cerr << "See verbosity level for more details." << std::endl;

        return EXIT_FAILURE;
    }
    int sn = cap.getSerialNumber();
    std::cout << "Connected to camera sn: " << sn << std::endl;
    // <---- Create Video Capture

    // ----> Retrieve calibration file from Stereolabs server
    std::string calibration_file;
    // ZED Calibration
    unsigned int serial_number = sn;
    // Download camera calibration file
    if( !sl_oc::tools::downloadCalibrationFile(serial_number, calibration_file) )
    {
        std::cerr << "Could not load calibration file from Stereolabs servers" << std::endl;
        return EXIT_FAILURE;
    }
    std::cout << "Calibration file found. Loading..." << std::endl;

    // ----> Frame size
    int w,h;
    cap.getFrameSize(w,h);
    // <---- Frame size

    // ----> Initialize calibration
    cv::Mat map_left_x, map_left_y;
    cv::Mat map_right_x, map_right_y;
    cv::Mat cameraMatrix_left, cameraMatrix_right;
    double baseline=0;
    sl_oc::tools::initCalibration(calibration_file, cv::Size(w/2,h), map_left_x, map_left_y, map_right_x, map_right_y,
                                  cameraMatrix_left, cameraMatrix_right, &baseline);

    double fx = cameraMatrix_left.at<double>(0,0);
    double fy = cameraMatrix_left.at<double>(1,1);
    double cx = cameraMatrix_left.at<double>(0,2);
    double cy = cameraMatrix_left.at<double>(1,2);

    std::cout << " Camera Matrix L: \n" << cameraMatrix_left << std::endl << std::endl;
    std::cout << " Camera Matrix R: \n" << cameraMatrix_right << std::endl << std::endl;

#ifdef USE_OCV_TAPI
    cv::UMat map_left_x_gpu = map_left_x.getUMat(cv::ACCESS_READ,cv::USAGE_ALLOCATE_DEVICE_MEMORY);
    cv::UMat map_left_y_gpu = map_left_y.getUMat(cv::ACCESS_READ,cv::USAGE_ALLOCATE_DEVICE_MEMORY);
    cv::UMat map_right_x_gpu = map_right_x.getUMat(cv::ACCESS_READ,cv::USAGE_ALLOCATE_DEVICE_MEMORY);
    cv::UMat map_right_y_gpu = map_right_y.getUMat(cv::ACCESS_READ,cv::USAGE_ALLOCATE_DEVICE_MEMORY);
#endif
    // ----> Initialize calibration

    // ----> Declare OpenCV images
#ifdef USE_OCV_TAPI
    cv::UMat frameYUV;  // Full frame side-by-side in YUV 4:2:2 format
    cv::UMat frameBGR(cv::USAGE_ALLOCATE_DEVICE_MEMORY); // Full frame side-by-side in BGR format
    cv::UMat left_raw(cv::USAGE_ALLOCATE_DEVICE_MEMORY); // Left unrectified image
    cv::UMat right_raw(cv::USAGE_ALLOCATE_DEVICE_MEMORY); // Right unrectified image
    cv::UMat left_rect(cv::USAGE_ALLOCATE_DEVICE_MEMORY); // Left rectified image
    cv::UMat right_rect(cv::USAGE_ALLOCATE_DEVICE_MEMORY); // Right rectified image
    cv::UMat left_for_matcher(cv::USAGE_ALLOCATE_DEVICE_MEMORY); // Left image for the stereo matcher
    cv::UMat right_for_matcher(cv::USAGE_ALLOCATE_DEVICE_MEMORY); // Right image for the stereo matcher
    cv::UMat left_disp_half(cv::USAGE_ALLOCATE_DEVICE_MEMORY); // Half sized disparity map
    cv::UMat left_disp(cv::USAGE_ALLOCATE_DEVICE_MEMORY); // Full output disparity
    cv::UMat left_disp_float(cv::USAGE_ALLOCATE_DEVICE_MEMORY); // Final disparity map in float32
    cv::UMat left_disp_image(cv::USAGE_ALLOCATE_DEVICE_MEMORY); // Normalized and color remapped disparity map to be saved
    cv::UMat left_depth_map(cv::USAGE_ALLOCATE_DEVICE_MEMORY); // Depth map in float32
#else
    cv::Mat frameBGR, left_raw, left_rect, right_raw, right_rect, frameYUV, left_for_matcher, right_for_matcher, left_disp_half,left_disp,left_disp_float, left_disp_vis;
#endif
    // <---- Declare OpenCV images

    // ----> Stereo matcher initialization
    sl_oc::tools::StereoSgbmPar stereoPar;

    //Note: you can use the tool 'zed_open_capture_depth_tune_stereo' to tune the parameters and save them to YAML
    if(!stereoPar.load())
    {
        stereoPar.save(); // Save default parameters.
    }

    cv::Ptr<cv::StereoSGBM> left_matcher = cv::StereoSGBM::create(stereoPar.minDisparity,stereoPar.numDisparities,stereoPar.blockSize);
    left_matcher->setMinDisparity(stereoPar.minDisparity);
    left_matcher->setNumDisparities(stereoPar.numDisparities);
    left_matcher->setBlockSize(stereoPar.blockSize);
    left_matcher->setP1(stereoPar.P1);
    left_matcher->setP2(stereoPar.P2);
    left_matcher->setDisp12MaxDiff(stereoPar.disp12MaxDiff);
    left_matcher->setMode(stereoPar.mode);
    left_matcher->setPreFilterCap(stereoPar.preFilterCap);
    left_matcher->setUniquenessRatio(stereoPar.uniquenessRatio);
    left_matcher->setSpeckleWindowSize(stereoPar.speckleWindowSize);
    left_matcher->setSpeckleRange(stereoPar.speckleRange);

    stereoPar.print();
    // <---- Stereo matcher initialization

    // ----> Point Cloud
    cv::Mat cloudMat;

    uint64_t last_ts=0; // Used to check new frame arrival

    // ----> Video Writer definition
    // Define the output video file name and codec
    std::string outputFilename = recording_name + ".avi";
    int fourcc = cv::VideoWriter::fourcc('M', 'J', 'P', 'G');  // Codec for gray AVI format

    // Create a VideoWriter object to write the video to a file
    cv::VideoWriter videoWriter(outputFilename, fourcc, (double) params.fps, cv::Size(w/2, h), false);

    // Check if the VideoWriter was successfully opened
    if (!videoWriter.isOpened()) {
        std::cerr << "Could not open the output video file for writing" << std::endl;
        return EXIT_FAILURE;
    }
    // <---- Video Writer definition

    // Infinite video grabbing loop
    for (int k=0; k<loop_count; k++)
    {
        auto startTime = std::chrono::high_resolution_clock::now();

        // Get a new frame from camera
        const sl_oc::video::Frame frame = cap.getLastFrame();

        // ----> If the frame is valid we can convert, rectify and display it
        if(frame.data!=nullptr && frame.timestamp!=last_ts)
        {
            last_ts = frame.timestamp;

            // ----> Conversion from YUV 4:2:2 to BGR for visualization
#ifdef USE_OCV_TAPI
            cv::Mat frameYUV_cpu = cv::Mat( frame.height, frame.width, CV_8UC2, frame.data );
            frameYUV = frameYUV_cpu.getUMat(cv::ACCESS_READ,cv::USAGE_ALLOCATE_HOST_MEMORY);
#else
            frameYUV = cv::Mat( frame.height, frame.width, CV_8UC2, frame.data );
#endif
            cv::cvtColor(frameYUV,frameBGR,cv::COLOR_YUV2BGR_YUYV);
            // <---- Conversion from YUV 4:2:2 to BGR for visualization

            // ----> Extract left and right images from side-by-side
            left_raw = frameBGR(cv::Rect(0, 0, frameBGR.cols / 2, frameBGR.rows));
            right_raw = frameBGR(cv::Rect(frameBGR.cols / 2, 0, frameBGR.cols / 2, frameBGR.rows));
            // <---- Extract left and right images from side-by-side

            // ----> Apply rectification
            sl_oc::tools::StopWatch remap_clock;
#ifdef USE_OCV_TAPI
            cv::remap(left_raw, left_rect, map_left_x_gpu, map_left_y_gpu, cv::INTER_AREA );
            cv::remap(right_raw, right_rect, map_right_x_gpu, map_right_y_gpu, cv::INTER_AREA );
#else
            cv::remap(left_raw, left_rect, map_left_x, map_left_y, cv::INTER_AREA );
            cv::remap(right_raw, right_rect, map_right_x, map_right_y, cv::INTER_AREA );
#endif
            double remap_elapsed = remap_clock.toc();
            std::stringstream remapElabInfo;
            remapElabInfo << "Rectif. processing: " << remap_elapsed << " sec - Freq: " << 1./remap_elapsed;
            // <---- Apply rectification

            // ----> Stereo matching
            sl_oc::tools::StopWatch stereo_clock;

            // Resize the original images to improve performances
            cv::resize(left_rect,  left_for_matcher,  cv::Size(), SCALE_FACTOR, SCALE_FACTOR, cv::INTER_AREA);
            cv::resize(right_rect, right_for_matcher, cv::Size(), SCALE_FACTOR, SCALE_FACTOR, cv::INTER_AREA);

            // Apply stereo matching
            left_matcher->compute(left_for_matcher, right_for_matcher, left_disp_half);

            left_disp_half.convertTo(left_disp_float,CV_32FC1);

            //cv::multiply(left_disp_float,1./16.,left_disp_float); // Last 4 bits of SGBM disparity are decimal
            //cv::multiply(left_disp_float,2.,left_disp_float); // Last 4 bits of SGBM disparity are decimal

            cv::UMat tmp = left_disp_float; // Required for OpenCV 3.2
            cv::resize(tmp, left_disp_float, cv::Size(), 1./SCALE_FACTOR, 1./SCALE_FACTOR, cv::INTER_AREA);

            double elapsed = stereo_clock.toc();
            std::stringstream stereoElabInfo;
            stereoElabInfo << "Stereo processing: " << elapsed << " sec - Freq: " << 1./elapsed;
            // <---- Stereo matching

            // ----> Write disparity image
            cv::add(left_disp_float,-static_cast<double>(stereoPar.minDisparity-1),left_disp_float); // Minimum disparity offset correction
            cv::multiply(left_disp_float,1./stereoPar.numDisparities,left_disp_image,255., CV_8UC1 ); // Normalization and rescaling

            // From gray to colorfull
            //cv::applyColorMap(left_disp_image,left_disp_image,cv::COLORMAP_JET); // COLORMAP_INFERNO is better, but it's only available starting from OpenCV v4.1.0

            videoWriter.write(left_disp_image);
            // <---- Write disparity image

            // ----> Extract Depth map
            // The DISPARITY MAP can be now transformed in DEPTH MAP using the formula
            // depth = (f * B) / disparity
            // where 'f' is the camera focal, 'B' is the camera baseline, 'disparity' is the pixel disparity

            //double num = static_cast<double>(fx*baseline);
            //cv::divide(num,left_disp_float,left_depth_map);

            // float central_depth = left_depth_map.getMat(cv::ACCESS_READ).at<float>(left_depth_map.rows/2, left_depth_map.cols/2 );
            // std::cout << "Depth of the central pixel: " << central_depth << " mm" << std::endl;
            // <---- Extract Depth map

            // ----> Create Point Cloud
            //sl_oc::tools::StopWatch pc_clock;
            //size_t buf_size = static_cast<size_t>(left_depth_map.cols * left_depth_map.rows);
            //std::vector<cv::Vec3d> buffer( buf_size, cv::Vec3f::all( std::numeric_limits<float>::quiet_NaN() ) );
            //cv::Mat depth_map_cpu = left_depth_map.getMat(cv::ACCESS_READ);
            //float* depth_vec = (float*)(&(depth_map_cpu.data[0]));

//#pragma omp parallel for
            //for(size_t idx=0; idx<buf_size;idx++ )
            //{
                //size_t r = idx/left_depth_map.cols;
                //size_t c = idx%left_depth_map.cols;
                //double depth = static_cast<double>(depth_vec[idx]);
                //std::cout << depth << " ";
                //if(!isinf(depth) && depth >=0 && depth > stereoPar.minDepth_mm && depth < stereoPar.maxDepth_mm)
                //{
                    //buffer[idx].val[2] = depth; // Z
                    //buffer[idx].val[0] = (c-cx)*depth/fx; // X
                    //buffer[idx].val[1] = (r-cy)*depth/fy; // Y
                //}
            //}

            //cloudMat = cv::Mat( left_depth_map.rows, left_depth_map.cols, CV_64FC3, &buffer[0] ).clone();

            //double pc_elapsed = stereo_clock.toc();
            //std::stringstream pcElabInfo;
            //pcElabInfo << "Point cloud processing: " << pc_elapsed << " sec - Freq: " << 1./pc_elapsed;
            //std::cout << pcElabInfo.str() << std::endl;
            // <---- Create Point Cloud
        }

        auto endTime = std::chrono::high_resolution_clock::now();
        auto elapsedTime = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

        if (elapsedTime < dt) {
            std::this_thread::sleep_for(dt - elapsedTime);
        } else
            std::cout << "Out of time..." << std::endl;

    }

    // Release VideoWriter object
    videoWriter.release();

    std::cout << "Video saved to " << outputFilename << std::endl;

    return EXIT_SUCCESS;
}
