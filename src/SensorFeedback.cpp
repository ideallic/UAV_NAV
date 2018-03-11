#include "DJI_guidance.h"
#include "DJI_utility.h"
#include "SensorFeedback.h"

ros::Publisher left_image_pub;
ros::Publisher right_image_pub;
ros::Publisher obstacle_distance_pub;
ros::Publisher ultrasonic_pub;

bool         show_info                   = true;
uint8_t      camera_select               = 0;
std::string  frame_id                    = "front";
e_vbus_index CAMERA_ID                   = e_vbus1;
DJI_lock     g_lock;
DJI_event    g_event;
cv::Mat      g_greyscale_image_left(HEIGHT, WIDTH, CV_8UC1);
cv::Mat      g_greyscale_image_right(HEIGHT, WIDTH, CV_8UC1);
cv::Mat      g_depth(HEIGHT,WIDTH,CV_16SC1);
cv::Mat      depth8(HEIGHT, WIDTH, CV_8UC1);

int main(int argc, char** argv) {
  // Initialize ROS
  ros::init(argc, argv, "SensorFeedback");
  ros::NodeHandle nh;
  left_image_pub			  = nh.advertise<sensor_msgs::Image>    ("/rob666/guidance/left_image",        1);
  right_image_pub			  = nh.advertise<sensor_msgs::Image>    ("/rob666/guidance/right_image",       1);
  obstacle_distance_pub	= nh.advertise<sensor_msgs::LaserScan>("/rob666/guidance/obstacle_distance", 1);
  ultrasonic_pub			  = nh.advertise<sensor_msgs::LaserScan>("/rob666/guidance/ultrasonic",        1);

  // Initialize Guidance
  reset_config();
  int err_code = init_transfer();
  RETURN_IF_ERR(err_code);

  // Check sensor online status
  int online_status[CAMERA_PAIR_NUM];
  err_code = get_online_status(online_status);
  RETURN_IF_ERR(err_code);
  std::cout<<"Sensor online status: ";
  for(int i=0; i<CAMERA_PAIR_NUM; i++) {
    std::cout<<online_status[i]<<" ";
  }
  std::cout<<std::endl;

  // Get calibration parameters
  stereo_cali cali[CAMERA_PAIR_NUM];
  err_code = get_stereo_cali(cali);
  RETURN_IF_ERR(err_code);
  std::cout<<"cu\tcv\tfocal\tbaseline\n";
  for(int i=0; i<CAMERA_PAIR_NUM; i++) {
    std::cout<<cali[i].cu<<"\t"<<cali[i].cv<<"\t"<<cali[i].focal<<"\t"<<cali[i].baseline<<std::endl;
  }

  // Select data
  err_code = select_greyscale_image(CAMERA_ID, true); //Left
  RETURN_IF_ERR(err_code);
  err_code = select_greyscale_image(CAMERA_ID, false); //Right
  RETURN_IF_ERR(err_code);
  err_code = select_depth_image(CAMERA_ID);
  RETURN_IF_ERR(err_code);
  select_ultrasonic();
  select_obstacle_distance();

  // Start data transfer
  std::cout<<"Starting data transfer"<<std::endl;
  err_code = set_sdk_event_handler(sensor_callback);
  RETURN_IF_ERR(err_code);
  err_code = start_transfer();
  RETURN_IF_ERR(err_code);

  while (ros::ok()) {
    g_event.wait_event();

    // Stop transfer in order to select new sensor
    err_code = stop_transfer();
    RETURN_IF_ERR(err_code);
    reset_config();

    switch (camera_select) {
      case 0:
        CAMERA_ID = e_vbus1;
        frame_id = "front";
        camera_select = 1;
        break;
      case 1:
        CAMERA_ID = e_vbus2;
        frame_id = "right";
        camera_select = 2;
        break;
      case 2:
        CAMERA_ID = e_vbus3;
        frame_id = "rear";
        camera_select = 3;
        break;
      case 3:
        CAMERA_ID = e_vbus4;
        frame_id = "left";
        camera_select = 4;
        break;
      case 4:
        CAMERA_ID = e_vbus5;
        frame_id = "down";
        camera_select = 0;
        break;
    }

    // Select data
    err_code = select_greyscale_image(CAMERA_ID, true); //Left
    RETURN_IF_ERR(err_code);
    err_code = select_greyscale_image(CAMERA_ID, false); //Right
    RETURN_IF_ERR(err_code);
    select_ultrasonic();
    select_obstacle_distance();

    // Start data transfer
    err_code = start_transfer();
    RETURN_IF_ERR(err_code);

    ros::Duration(0.053).sleep();
    ros::spinOnce();
  }

  // Release data transfer
  std::cout << "Stopping transfer" << std::endl;
  err_code = stop_transfer();
  RETURN_IF_ERR(err_code);
  sleep(1); // Wait for ACK packet from Guidance
  err_code = release_transfer();
  RETURN_IF_ERR(err_code);

  return 0;
}

int sensor_callback(int data_type, int data_len, char *content) {
  g_lock.enter();

  // Image data
  if(e_image == data_type && NULL != content) {
    image_data* data = (image_data*)content;

    if(data->m_greyscale_image_left[CAMERA_ID]) {
      memcpy(g_greyscale_image_left.data, data->m_greyscale_image_left[CAMERA_ID], IMAGE_SIZE);

      if(show_info) {
        imshow("left",  g_greyscale_image_left);
      }

      // Publish left greyscale image
      cv_bridge::CvImage left_8;
      g_greyscale_image_left.copyTo(left_8.image);
      left_8.header.frame_id = frame_id;
      left_8.header.stamp = ros::Time::now();
      left_8.encoding = sensor_msgs::image_encodings::MONO8;
      left_image_pub.publish(left_8.toImageMsg());
    }

    if(data->m_greyscale_image_right[CAMERA_ID]) {
      memcpy(g_greyscale_image_right.data, data->m_greyscale_image_right[CAMERA_ID], IMAGE_SIZE);
      if(show_info) {
        imshow("right", g_greyscale_image_right);
      }

      // Publish right greyscale image
      cv_bridge::CvImage right_8;
      g_greyscale_image_right.copyTo(right_8.image);
      right_8.header.frame_id = frame_id;
      right_8.header.stamp = ros::Time::now();
      right_8.encoding = sensor_msgs::image_encodings::MONO8;
      right_image_pub.publish(right_8.toImageMsg());
    }

    cv::waitKey(1);
  }

  // Obstacle distance
  if(e_obstacle_distance == data_type && NULL != content) {
    obstacle_distance *oa = (obstacle_distance*)content;

    if(show_info) {
      printf("frame index: %d, stamp: %d\n", oa->frame_index, oa->time_stamp);
      printf("obstacle distance:");
      for(int i = 0; i < CAMERA_PAIR_NUM; ++i) {
        printf(" %f ", 0.01f * oa->distance[i]);
      }
      printf( "\n" );
    }

    // Publish obstacle distance
    sensor_msgs::LaserScan g_oa;
    g_oa.ranges.resize(CAMERA_PAIR_NUM);
    g_oa.header.frame_id = frame_id;
    g_oa.header.stamp = ros::Time::now();
    for(int i = 0; i < CAMERA_PAIR_NUM; ++i) {
      g_oa.ranges[i] = 0.01f * oa->distance[i];
    }
    obstacle_distance_pub.publish(g_oa);
  }

  // Ultrasonic
  if(e_ultrasonic == data_type && NULL != content) {
    ultrasonic_data *ultrasonic = (ultrasonic_data*)content;

    if(show_info) {
      printf("frame index: %d, stamp: %d\n", ultrasonic->frame_index, ultrasonic->time_stamp);
      for(int d = 0; d < CAMERA_PAIR_NUM; ++d) {
        printf("ultrasonic distance: %f, reliability: %d\n", ultrasonic->ultrasonic[d] * 0.001f, (int)ultrasonic->reliability[d]);
      }
    }

    // Publish ultrasonic data
    sensor_msgs::LaserScan g_ul;
    g_ul.ranges.resize(CAMERA_PAIR_NUM);
    g_ul.intensities.resize(CAMERA_PAIR_NUM);
    g_ul.header.frame_id = frame_id;
    g_ul.header.stamp = ros::Time::now();
    for(int d = 0; d < CAMERA_PAIR_NUM; ++d) {
      g_ul.ranges[d] = 0.001f * ultrasonic->ultrasonic[d];
      g_ul.intensities[d] = 1.0 * ultrasonic->reliability[d];
    }
    ultrasonic_pub.publish(g_ul);
  }

  g_lock.leave();
  g_event.set_event();

  return 0;
}
