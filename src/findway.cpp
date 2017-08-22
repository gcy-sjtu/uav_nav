float SPD_MAX = 0.5;
float SAFE_RANGE = 0.8;
float STOP_RANGE = 0.2;
float MAP_ORIGIN_TO_STRUCT_X = 1.0;
float MAP_ORIGIN_TO_STRUCT_Y = 1.0;
float SCAN_DELAY = 2.0;
float SCAN_SPEED = 1.0;
float BRAKE_COE = 1.0;
float HEIGHT = 1.0;
float SPD_CLIMB = 0.1;
float SPD_TURN = 0.5;
float ANGLE_TOLERANCE = 0.3;
float Z_TOLERANCE = 0.05;
int WAYPOINT_GROUP = 1;

const double PI = 3.1415926;

#include "ros/ros.h"

#include <std_msgs/String.h>
#include <std_msgs/UInt8.h>
#include <geometry_msgs/Twist.h>
#include <geometry_msgs/PoseStamped.h>
#include <geometry_msgs/PoseWithCovarianceStamped.h>
#include <mavros_msgs/CameraPose.h>
#include <px4_autonomy/Takeoff.h>
#include <px4_autonomy/Velocity.h>
#include <px4_autonomy/Position.h>

#include <sstream>

ros::Publisher pub_pose_px4;

float last_set_x, last_set_y, last_set_z, last_set_yaw;
void sendPose_px4(float px, float py, float pz, float yaw) {
	px4_autonomy::Position pos;
	pos.x =-py;
	pos.y = px;
	pos.z = pz;
	yaw += PI/2;
	if (yaw > PI) {
		yaw -= PI * 2;
	}
	pos.yaw = yaw;
	pos.header.stamp = ros::Time::now();
	pub_pose_px4.publish(pos);
	last_set_x = px;
	last_set_y = py;
	last_set_z = pz;
	last_set_yaw = yaw;
}

bool gotlaser = false;
float laser_x, laser_y, laser_z, laser_yaw;
void laserPoseCB(const geometry_msgs::PoseStamped &msg) {
	gotlaser = true;
	laser_x = msg.pose.position.x;
	laser_y = msg.pose.position.y;
	laser_z = msg.pose.position.z;
	laser_yaw = (msg.pose.orientation.z) * 2;
}

float pose_x, pose_y, pose_z, pose_yaw;
void poseCB(const px4_autonomy::Position &msg) {
	pose_x = msg.y;
	pose_y =-msg.x;
	pose_z = msg.z;
	float yaw = msg.yaw;
	yaw -= PI/2;
	if (yaw < -PI) {
		yaw += PI * 2;
	}
	pose_yaw = yaw;
}

int status;
void statusCB(const std_msgs::UInt8::ConstPtr& msg) {
	status = msg -> data;
}

typedef struct _pos_points_s
{
	float x, y, z, yaw;
	bool change_z;
	bool change_yaw;
	float delay;
	bool need_delay;
	bool need_scan;
} _pos_points_t;

int progress = 0;
int numofWayPoints = 0;
_pos_points_t wayPoints[50];

void setZ(float z) {
	wayPoints[numofWayPoints - 1].change_z = true;
	wayPoints[numofWayPoints - 1].z = z;
}

void setYaw(float yaw) {
	wayPoints[numofWayPoints - 1].change_yaw = true;
	wayPoints[numofWayPoints - 1].yaw = yaw;
}

void setScan() {
	wayPoints[numofWayPoints - 1].need_scan = true;
}

void setDelay(float delay) {
	wayPoints[numofWayPoints - 1].need_delay = true;
	wayPoints[numofWayPoints - 1].delay = delay;
}

void addPosPoint(float x, float y) {
	_pos_points_t pp;
	pp.x = x + MAP_ORIGIN_TO_STRUCT_X;
	pp.y = y + MAP_ORIGIN_TO_STRUCT_Y;
	if (numofWayPoints != 0) {
		pp.z = wayPoints[numofWayPoints - 1].z;
		pp.yaw = wayPoints[numofWayPoints - 1].yaw;
	} else {
		pp.z = HEIGHT;
		pp.yaw = 0;
	}
	pp.change_z = false;
	pp.change_yaw = false;
	pp.need_scan = false;
	wayPoints[numofWayPoints] = pp;
	numofWayPoints ++;

	setDelay(1);	//default delay 1 sec
}

bool wayPointInit();

bool arrived_z(float from, float to) {
	if (fabs(from - to) < Z_TOLERANCE) {
		return true;
	} else {
		return false;
	}
}

bool arrived_yaw(float from, float to) {
	if ( fabs(from - to) < ANGLE_TOLERANCE) {
		return true;
	} else {
		return false;
	}
}

void turnYaw(float yaw_from, float yaw_to) {
	float turn_ori;
	float yaw_diff = yaw_to - yaw_from;
	if (yaw_diff > PI || (yaw_diff < 0 && yaw_diff > -PI)) {
		turn_ori = -1;
	} else {
		turn_ori = 1;
	}

	int counts = 1;
	ros::spinOnce();

	while (!arrived_yaw( laser_yaw, yaw_to)) {
		float yaw2set = yaw_from + turn_ori * SPD_TURN * 0.05 * counts;
		if (yaw2set < -PI) {
			yaw2set += 2 * PI;
		}
		if (yaw2set > PI) {
			yaw2set -= 2 * PI;
		}
		if ( arrived_yaw( yaw2set, yaw_to)) {
			yaw2set = yaw_to;
		}
		sendPose_px4(last_set_x, last_set_y, last_set_z, yaw2set);
		ros::Duration(0.05).sleep();
		counts ++;
		ros::spinOnce();
	}
	ROS_INFO("turn finished");
}

void climbZ(float z_from, float z_to) {
	float climb_ori;
	if (z_from > z_to) {
		climb_ori = -1;
	} else {
		climb_ori = 1;
	}

	int counts;
	ros::spinOnce();

	while (!arrived_z( laser_z, z_to)) {
		float z2set = z_from + climb_ori * SPD_CLIMB * 0.05 * counts;
		if (arrived_z( z2set, z_to)) {
			z2set = z_to;
		}
		sendPose_px4(last_set_x, last_set_y, z2set, last_set_yaw);
		ros::Duration(0.05).sleep();
		counts ++;
		ros::spinOnce();
	}
	ROS_INFO("climb finished");
}

ros::Publisher pub_takeoff;

void takeOff() {
	while (status != 1) {
		ros::Duration(1).sleep();
		ROS_INFO("waitting fir OFFBOARD");
		ros::spinOnce();
	}

	px4_autonomy::Takeoff cmd_tf;
	cmd_tf.take_off = 1;
	cmd_tf.header.stamp = ros::Time::now();
	pub_takeoff.publish(cmd_tf);

	while (status != 5) {
		ros::Duration(1).sleep();
		ROS_INFO("taking off...");
		ros::spinOnce();
	}
	ros::Duration(1).sleep();
	ROS_INFO("Start climb");
	climbZ(pose_z, HEIGHT);
}

void land() {
	px4_autonomy::Takeoff cmd_tf;
	cmd_tf.take_off = 2;
	cmd_tf.header.stamp = ros::Time::now();
	pub_takeoff.publish(cmd_tf);

	while (status != 1) {
		ros::Duration(1).sleep();
		ROS_INFO("landing...");
		ros::spinOnce();
	}

	ROS_INFO("landed");
}

void scan() {
;
}

bool needSetStartXY = false;
float start_x, start_y;
float spdx, spdy;
int counts_XY;
void setStartXY(_pos_points_t wp) {
	if (needSetStartXY) {
		float disx = wp.x - start_x;
		float disy = wp.y - start_y;
		float dis = sqrt(disx * disx + disy + disy);
		spdx = SPD_MAX / dis * disx;
		spdy = SPD_MAX / dis * disy;
		start_x = pose_x;
		start_y = pose_y;
		counts_XY = 1;
	}
}

int main(int argc, char **argv) {
	ros::init(argc, argv, "uav_nav");
	ros::NodeHandle n;

	n.getParam("/uav_nav/spd_max", SPD_MAX);
	n.getParam("/uav_nav/origin_x", MAP_ORIGIN_TO_STRUCT_X);
	n.getParam("/uav_nav/origin_y", MAP_ORIGIN_TO_STRUCT_Y);
	n.getParam("/uav_nav/scan_spd", SCAN_SPEED);
	n.getParam("/uav_nav/scan_delay", SCAN_DELAY);
	n.getParam("/uav_nav/stop_range", STOP_RANGE);
	n.getParam("/uav_nav/safe_range", SAFE_RANGE);
	n.getParam("/uav_nav/brake_coe", BRAKE_COE);
	n.getParam("/uav_nav/waypoint_group", WAYPOINT_GROUP);
	n.getParam("/uav_nav/height", HEIGHT);
	n.getParam("/uav_nav/spd_climb", SPD_CLIMB);
	n.getParam("/uav_nav/spd_turn", SPD_TURN);
	n.getParam("/uav_nav/angle_tolerance", ANGLE_TOLERANCE);
	n.getParam("/uav_nav/z_tolerance", Z_TOLERANCE);

	if (!wayPointInit()) return 0;

	pub_pose_px4 = n.advertise<px4_autonomy::Position>("/px4/cmd_pos",1);
	pub_takeoff = n.advertise<px4_autonomy::Takeoff>("/px4/cmd_takeoff", 1);

	ros::Subscriber sub_laser = n.subscribe("/mavros/vision_pose/pose", 1, laserPoseCB);
	ros::Subscriber sub_pose = n.subscribe("/px4/pose", 1, poseCB);	
	ros::Subscriber sub_status = n.subscribe("/px4/status", 1, statusCB);

	ros::Rate loop_rate(20);

	while (!gotlaser) {
		ros::spinOnce();
		ROS_INFO("waitting for amcl_pose");
		ros::Duration(1).sleep();
	}

	last_set_x = laser_x;
	last_set_y = laser_y;
	last_set_yaw = 0;

	ROS_INFO("get laser, start guide");

	takeOff();

	ROS_INFO("take off complete");
	ros::Duration(1).sleep();

	needSetStartXY = true;

	while (ros::ok()) {
		ros::spinOnce();
		
		_pos_points_t wp = wayPoints[progress];
		setStartXY(wp);

		float dis_now_x = wp.x - pose_x;
		float dis_now_y = wp.y - pose_y;
		float dis_now = sqrt(dis_now_x * dis_now_x + dis_now_y + dis_now_y);

		if (dis_now < STOP_RANGE) {
			ROS_INFO("Reach Point NO.%i:  %.3f\t%.3f", 
				progress, wp.x, wp.y);

			if (wp.need_delay) {
				ros::Duration(wp.delay).sleep();
			}

			if (wp.change_yaw) {
				float from = pose_yaw;
				float to = wp.yaw;
				ROS_INFO("need turn\tFrom %.3f\tto %.3f", from, to);
				turnYaw(from, to);
			}

			if (wp.change_z) {
				float from = pose_z;
				float to = wp.z;
				ROS_INFO("need climb\tFrom %.3f\tto %.3f", from, to);
				climbZ(from, to);
			}

			if (wp.need_scan) {
				ROS_INFO("need scan");
				scan();
			}

			progress ++;

			if ( progress >= numofWayPoints) {
				ROS_INFO("nav finished");
				ros::Duration(3).sleep();
				ROS_INFO("start land");
				ros::spinOnce();
				land();
				ros::spin();
			}

			needSetStartXY = true;

			ROS_INFO("NEXT: \t%.3f\t%.3f\txplus:%.3f\typlus:%.3f\n"
			, wayPoints[progress+1].x, wayPoints[progress+1].y,
			wayPoints[progress+1].x - wp.x,
			wayPoints[progress+1].y - wp.y);

			continue;
		}


		float x2set = start_x + spdx * 0.05 * counts_XY;
		float y2set = start_y + spdy * 0.05 * counts_XY;
		float dis_set_x = wp.x - x2set;
		float dis_set_y = wp.y - y2set;
		float dis_set = sqrt(dis_set_x * dis_set_x + dis_set_y + dis_set_y);
		if (dis_set < STOP_RANGE) {
			x2set = wp.x;
			y2set = wp.y;
		}
		sendPose_px4(x2set, y2set, last_set_z, last_set_yaw);
		counts_XY++;

		loop_rate.sleep();
	}

	return 0;
}

bool wayPointInit() {
	switch(WAYPOINT_GROUP) {
		case 5:
			addPosPoint( 1.00, 1.00);
			addPosPoint( 4.00, 1.00); setZ(1.3);
			addPosPoint( 4.40, 0.00); setYaw(3.1);
			addPosPoint( 5.25,-1.00); setZ(0.8);
			addPosPoint( 7.36,-1.20);
			addPosPoint( 7.34, 0.67);
			addPosPoint( 7.25,-1.20);
			addPosPoint( 4.56,-0.83); setYaw(1.57);
			addPosPoint( 4.28, 2.00);
			addPosPoint( 5.07, 3.00);
			addPosPoint( 7.42, 2.92);
			addPosPoint( 7.93, 4.41);
			addPosPoint( 7.48, 2.90);
			addPosPoint( 5.62, 2.83);
			addPosPoint( 5.73, 5.00);
			addPosPoint( 5.82, 5.80);
			addPosPoint( 6.10, 7.00); setYaw(-1.57);
			addPosPoint( 7.40, 7.79);
			addPosPoint( 5.80, 8.80);
			addPosPoint( 3.93, 9.10);
			addPosPoint( 3.43, 7.55); setYaw(0);
			addPosPoint( 2.27, 6.87); setYaw(3.1);
			addPosPoint( 1.21, 7.75);
			addPosPoint( 1.33, 9.22);
			addPosPoint(-0.15, 8.80);
			break;
		default:
			ROS_ERROR("no such waypoint group!");
			return false;
	}
	return true;
}