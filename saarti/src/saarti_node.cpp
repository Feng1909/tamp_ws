#include "saarti/saarti_node.h"

namespace saarti_node{

SAARTI::SAARTI(ros::NodeHandle nh){
    nh_ = nh;
    dt = 0.1;
    ros::Rate loop_rate(1/dt);

    // pubs & subs
    trajhat_pub_ = nh.advertise<common::Trajectory>("trajhat",1);
    trajstar_pub_ = nh.advertise<common::Trajectory>("trajstar",1);
    pathlocal_sub_ = nh.subscribe("pathlocal", 1, &SAARTI::pathlocal_callback,this);
    obstacles_sub_ = nh.subscribe("obstacles", 1, &SAARTI::obstacles_callback,this);
    state_sub_ = nh.subscribe("state", 1,  &SAARTI::state_callback,this);
    // visualization
    trajhat_vis_pub_ = nh.advertise<nav_msgs::Path>("trajhat_vis",1);
    trajstar_vis_pub_ = nh.advertise<nav_msgs::Path>("trajstar_vis",1);
    trajset_vis_pub_ = nh.advertise<visualization_msgs::MarkerArray>("trajset_vis",1);
    posconstr_vis_pub_ = nh.advertise<jsk_recognition_msgs::PolygonArray>("posconstr_vis",1);

    // init wrapper for rtisqp solver
    rtisqp_wrapper_ = RtisqpWrapper();

    // set weights
    rtisqp_wrapper_.setWeights(Wx,Wu,Wslack);

    // wait until state and path_local is received
    while( (state_.s <= 0) || pathlocal_.s.size() == 0 ){
        ROS_INFO_STREAM("waiting for state and path local");
        ros::spinOnce();
        loop_rate.sleep();
    }

    // initialize trajhat last
    planning_util::trajstruct trajstar_last;

    // main loop
    while (ros::ok())
    {
        ROS_INFO_STREAM(" ");
        ROS_INFO_STREAM("main_ loop_");
        auto t1_loop = std::chrono::high_resolution_clock::now();

        // Todo: update traction map

        /*
         * FEASIBLE TRAJECTORY ROLLOUT
         */

        // set refs
        refs_ = setRefs(ctrl_mode_); // 0: tracking(unused todo remove), 1: min s, 2: max s,

        // rollout
        ROS_INFO_STREAM("generating trajectory set");
        trajset_.clear();
        int Nsamples = 6;
        auto t1_rollout = std::chrono::high_resolution_clock::now();
        rtisqp_wrapper_.computeTrajset(trajset_,
                                       state_,
                                       pathlocal_,
                                       uint(Nsamples));
        auto t2_rollout = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::milli> t_rollout = t2_rollout - t1_rollout;

        // append fwd shifted trajstar last
        if(trajstar_last.s.size()>0){
            rtisqp_wrapper_.shiftTrajectoryFwdSimple(trajstar_last);
            trajset_.push_back(trajstar_last);
        }
        trajset2cart(); // only for visualization, comment out to save time
        trajset2ma();

        // cost eval and select
        int trajhat_idx = trajset_eval_cost(); // error if negative

        planning_util::trajstruct trajhat;
        if(trajhat_idx >= 0){
            trajhat = trajset_.at(uint(trajhat_idx));
        } else {
            ROS_ERROR("no traj selected");
        }
        ROS_INFO_STREAM("trajhat_idx = " << trajhat_idx);
        ROS_INFO_STREAM("trajhat.cost = " << trajhat.cost);
        nav_msgs::Path p_trajhat = traj2navpath(trajhat);
        if(trajhat.s.back() > 0.95f*pathlocal_.s.back()){
            ROS_WARN_STREAM("Running out of path!");
        }

        /*
         * OPTIMIZATION
         */

        // update adaptive constraints for opt
        rtisqp_wrapper_.setInputConstraints(0.5,1000);

        // update state for opt
        ROS_INFO_STREAM("setting state..");
        rtisqp_wrapper_.setInitialState(state_);

        // set initial guess and shift fwd
        ROS_INFO_STREAM("setting initial guess..");
        rtisqp_wrapper_.setInitialGuess(trajhat);
        //print_obj(trajhat);
        //rtisqp_wrapper_.shiftStateAndControls();

        // set refs in solver
        ROS_INFO_STREAM("setting reference..");
        rtisqp_wrapper_.setOptReference(trajhat,refs_);

        // set state constraint
        ROS_INFO_STREAM("setting state constraints..");
        vector<float> lld = cpp_utils::interp(trajhat.s,pathlocal_.s,pathlocal_.dub,false);
        vector<float> rld = cpp_utils::interp(trajhat.s,pathlocal_.s,pathlocal_.dlb,false);
        planning_util::posconstrstruct posconstr = rtisqp_wrapper_.setStateConstraints(trajhat,obst_,lld,rld);
        // visualize state constraint
        jsk_recognition_msgs::PolygonArray polarr = stateconstr2polarr(posconstr);

        auto t1_opt = std::chrono::high_resolution_clock::now();
        // do preparation step
        ROS_INFO_STREAM("calling acado prep step..");
        rtisqp_wrapper_.doPreparationStep();

        // do feedback step
        ROS_INFO_STREAM("calling acado feedback step..");
        int status = rtisqp_wrapper_.doFeedbackStep();
        if (status){
            cout << "QP problem! QP status: " << status << endl;
            break;
        }
        auto t2_opt = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::milli> t_opt = t2_opt - t1_opt;

        // extract trajstar from acado
        planning_util::trajstruct trajstar = rtisqp_wrapper_.getTrajectory();
        traj2cart(trajstar);
        nav_msgs::Path p_trajstar = traj2navpath(trajstar);

        /*
         * PUBLISH
         */

        // publish trajhat
        common::Trajectory trajhat_msg = traj2msg(trajhat);
        trajhat_msg.slb = posconstr.slb;
        trajhat_msg.sub = posconstr.sub;
        trajhat_msg.dlb = posconstr.dlb;
        trajhat_msg.dub = posconstr.dub;
        trajhat_msg.header.stamp = ros::Time::now();
        trajhat_pub_.publish(trajhat_msg);

        // publish trajstar
        common::Trajectory trajstar_msg = traj2msg(trajstar);
        trajstar_msg.header.stamp = ros::Time::now();
        trajstar_pub_.publish(trajstar_msg);

        // publish visualization msgs
        trajhat_vis_pub_.publish(p_trajhat);
        trajstar_vis_pub_.publish(p_trajstar);
        trajset_vis_pub_.publish(trajset_ma_);
        posconstr_vis_pub_.publish(polarr);

        // store trajstar for next iteration
        trajstar_last = trajstar;

        // print timings
        ROS_INFO_STREAM("planning iteration complete, Timings: ");
        auto t2_loop = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::milli> t_loop = t2_loop - t1_loop;
        if(t_loop.count() > dt*1000 ){
            ROS_WARN_STREAM("looptime exceeding dt! looptime is " << t_loop.count() << " ms ");
        } else{
            ROS_INFO_STREAM("looptime is " << t_loop.count() << " ms ");
        }
        ROS_INFO_STREAM("rollout took                 " << t_rollout.count() << " ms ");
        ROS_INFO_STREAM("optimization took            " << t_opt.count() << " ms ");

        ros::spinOnce();
        loop_rate.sleep();
    }
}


/*
 * FUNCTIONS
 */


// print size of object for debugging
void SAARTI::print_obj(planning_util::trajstruct traj){
    // state
    cout << "length of s: " << traj.s.size() << endl;
    cout << "length of d: " << traj.d.size() << endl;
    cout << "length of deltapsi: " << traj.deltapsi.size() << endl;
    cout << "length of psidot: " << traj.psidot.size() << endl;
    cout << "length of vx: " << traj.vx.size() << endl;
    cout << "length of vy: " << traj.vy.size() << endl;
    // control
    cout << "length of Fyf: " << traj.Fyf.size() << endl;
    cout << "length of Fx: " << traj.Fx.size() << endl;
    // cartesian pose
    cout << "length of X: " << traj.X.size() << endl;
    cout << "length of Y: " << traj.Y.size() << endl;
    cout << "length of psi: " << traj.psi.size() << endl;
}

// sets refs to be used in rollout and optimization
planning_util::refstruct SAARTI::setRefs(uint ctrlmode){
    planning_util::refstruct refs;
    switch (ctrlmode) {
    case 1:  // minimize vx (emg brake)
        refs.sref.assign(N+1,state_.s);
        refs.vxref.assign(N+1,0.0);
        break;
    case 2: // maximize s (racing)
        refs.sref.assign(N+1, state_.s + 300);
        refs.vxref.assign(N+1, state_.vx + 25);
        break;
    }
    return refs;
}

// computes cartesian coordinates of a trajectory
void SAARTI::traj2cart(planning_util::trajstruct &traj){
    if(!traj.s.size()){
        ROS_ERROR("traj2cart on traj of 0 length");
    }
    else {
        vector<float> Xc = cpp_utils::interp(traj.s,pathlocal_.s,pathlocal_.X,false);
        vector<float> Yc = cpp_utils::interp(traj.s,pathlocal_.s,pathlocal_.Y,false);
        vector<float> psic = cpp_utils::interp(traj.s,pathlocal_.s,pathlocal_.psi_c,false);
        for (uint j=0; j<traj.s.size();j++) {
            if(std::isnan(traj.s.at(j))){
                ROS_ERROR("trajectory has nans");
            }
            // X = Xc - d*sin(psic);
            // Y = Yc + d*cos(psic);
            // psi = deltapsi + psic;
            float X = Xc.at(j) - traj.d.at(j)*std::sin(psic.at(j));
            float Y = Yc.at(j) + traj.d.at(j)*std::cos(psic.at(j));
            float psi = traj.deltapsi.at(j) + psic.at(j);
            traj.X.push_back(X);
            traj.Y.push_back(Y);
            traj.psi.push_back(psi);
        }
        traj.kappac = cpp_utils::interp(traj.s,pathlocal_.s,pathlocal_.kappa_c,false);
    }
}

// computes cartesian coordinates of a trajectory set
void SAARTI::trajset2cart(){
    for (uint i=0;i<trajset_.size();i++) {
        traj2cart(trajset_.at(i));
    }
}

// computes cartesian coordinates of a set of s,d pts
void SAARTI::sd_pts2cart(vector<float> &s, vector<float> &d, vector<float> &Xout, vector<float> &Yout){
    vector<float> Xc = cpp_utils::interp(s,pathlocal_.s,pathlocal_.X,false);
    vector<float> Yc = cpp_utils::interp(s,pathlocal_.s,pathlocal_.Y,false);
    vector<float> psic = cpp_utils::interp(s,pathlocal_.s,pathlocal_.psi_c,false);
    for (uint j=0; j<s.size();j++) {
        // X = Xc - d*sin(psic);
        // Y = Yc + d*cos(psic);
        // psi = deltapsi + psic;
        float X = Xc.at(j) - d.at(j)*std::sin(psic.at(j));
        float Y = Yc.at(j) + d.at(j)*std::cos(psic.at(j));
        Xout.push_back(X);
        Yout.push_back(Y);
    }
}

// cost evaluation and collision checking of trajset
int SAARTI::trajset_eval_cost(){
    float mincost = float(Wslack)*10;
    int trajhat_idx = -1;
    for (uint i=0;i<trajset_.size();i++) {
        planning_util::trajstruct traj = trajset_.at(i);
        bool colliding = false;
        bool exitroad = false;
        float cost = 0;
        vector<float> dub = cpp_utils::interp(traj.s,pathlocal_.s,pathlocal_.dub,false);
        vector<float> dlb = cpp_utils::interp(traj.s,pathlocal_.s,pathlocal_.dlb,false);
        for (uint j=0; j<traj.s.size();j++){
            float s = traj.s.at(j);
            float d = traj.d.at(j);
            float vx = traj.vx.at(j);
            // check obstacle (in frenet)
            float dist;
            for (uint k=0; k<obst_.s.size();k++){
                dist = std::sqrt( (s-obst_.s.at(k))*(s-obst_.s.at(k)) + (d-obst_.d.at(k))*(d-obst_.d.at(k)) );
                if(dist < obst_.Rmgn.at(k)){
                    colliding = true;
                }
            }
            // check outside road (in frenet)
            if((d > dub.at(j)) || d < dlb.at(j) ){
                exitroad = true;
            }
            // running cost
            float sref = float(refs_.sref.at(j));
            float vxref = float(refs_.vxref.at(j));
            //cout << "sref before rc add = " << sref << endl;
            //cout << "vxref before rc add = " << vxref << endl;
            //cout << "s before rc add = " << s << endl;
            //cout << "vx before rc add = " << vx << endl;
            //cout << "cost before rc add = " << cost << endl;
            cost += (sref-s)*float(Wx.at(0))*(sref-s) + (vxref-vx)*float(Wx.at(4))*(vxref-vx);
            //cout << "cost after rc add = " << cost << endl;
        }
        if(colliding){
            cost += float(Wslack);
            //cost = float(Wslack);
        }
        if(exitroad){
            cost += float(Wslack);
            //cost = float(Wslack);
        }
        traj.cost = cost;
        //cout << "cost of traj nr " << i << ": " << cost << endl;
        traj.colliding = colliding;
        traj.exitroad = exitroad;

        // keep track of minimum cost traj
        if(cost < mincost){
            mincost = cost;
            trajhat_idx = int(i);
        }
    }
    return trajhat_idx;
}

common::Trajectory SAARTI::traj2msg(planning_util::trajstruct traj){
    common::Trajectory trajmsg;
    // state
    trajmsg.s = traj.s;
    trajmsg.d = traj.d;
    trajmsg.deltapsi = traj.deltapsi;
    trajmsg.psidot = traj.psidot;
    trajmsg.vx = traj.vx;
    trajmsg.vy = traj.vy;
    // ctrl
    trajmsg.Fyf = traj.Fyf;
    trajmsg.Fx = traj.Fx;
    // cart pose
    trajmsg.X = traj.X;
    trajmsg.Y = traj.Y;
    trajmsg.psi = traj.psi;

    return trajmsg;
}

// represent traj as navmsgs path for visualization
nav_msgs::Path SAARTI::traj2navpath(planning_util::trajstruct traj){
    nav_msgs::Path p;
    p.header.stamp = ros::Time::now();
    p.header.frame_id = "map";
    for (uint i=0;i<traj.s.size();i++){
        geometry_msgs::PoseStamped pose;
        pose.header.stamp = ros::Time::now();
        pose.header.frame_id = "map";
        pose.pose.position.x = double(traj.X.at(i));
        pose.pose.position.y = double(traj.Y.at(i));
        tf2::Quaternion q;
        q.setRPY(0,0,double(traj.psi.at(i)));
        pose.pose.orientation.w = q.w();
        pose.pose.orientation.x = q.x();
        pose.pose.orientation.y = q.y();
        pose.pose.orientation.z = q.z();
        p.poses.push_back(pose);
    }
    return p;
}

// fill trajset marker array for visualization
void SAARTI::trajset2ma(){
    trajset_ma_.markers.clear();
    int count = 0;
    for (uint i=0; i<trajset_.size(); i++) {
        planning_util::trajstruct traj = trajset_.at(i);
        for (uint j=0; j<traj.s.size();j++) {
            visualization_msgs::Marker m;
            m.header.stamp = ros::Time::now();
            m.header.frame_id = "map";
            m.id = count;
            m.pose.position.x = double(traj.X.at(j));
            m.pose.position.y = double(traj.Y.at(j));
            // style
            m.type = m.CUBE;
            m.scale.x = 0.1;
            m.scale.y = 0.1;
            m.scale.z = 0.01;
            m.color.a = 1.0;
            m.color.r = 0.0;
            m.color.g = 1.0;
            m.color.b = 0.0;
            trajset_ma_.markers.push_back(m);
            count++;
        }
    }
}

// create visualization obj for state constraints
jsk_recognition_msgs::PolygonArray SAARTI::stateconstr2polarr(planning_util::posconstrstruct pc){
    jsk_recognition_msgs::PolygonArray polarr;
    polarr.header.stamp = ros::Time::now();
    polarr.header.frame_id = "map";
    for (uint i=0;i<pc.dlb.size();i++){
        geometry_msgs::PolygonStamped poly;
        poly.header.stamp = ros::Time::now();
        poly.header.frame_id = "map";
        vector<float> s{pc.slb.at(i),pc.sub.at(i),pc.sub.at(i),pc.slb.at(i)};
        vector<float> d{pc.dub.at(i),pc.dub.at(i),pc.dlb.at(i),pc.dlb.at(i)};
        vector<float> X;
        vector<float> Y;
        sd_pts2cart(s, d, X, Y);
        for (uint j=0;j<4;j++){
            geometry_msgs::Point32 pt;
            pt.x = X.at(j);
            pt.y = Y.at(j);
            poly.polygon.points.push_back(pt);
        }
        polarr.polygons.push_back(poly);
    }
    return polarr;
}

void SAARTI::state_callback(const common::State::ConstPtr& msg){
    state_.s = msg->s;
    state_.d = msg->d;
    state_.deltapsi = msg->deltapsi;
    state_.psidot = msg->psidot;
    state_.vx = msg->vx;
    state_.vy = msg->vy;

    // curvilinear dynamics breaks when vx == 0
    if (state_.vx <= 0.1f){
        state_.vx = 0.1f;
    }
}

void SAARTI::pathlocal_callback(const common::Path::ConstPtr& msg){
    pathlocal_.X = msg->X;
    pathlocal_.Y = msg->Y;
    pathlocal_.s = msg->s;
    pathlocal_.psi_c = msg->psi_c;
    pathlocal_.kappa_c = msg->kappa_c;
    pathlocal_.theta_c = msg->theta_c;
    pathlocal_.psi_c = msg->psi_c;
    pathlocal_.dub = msg->dub;
    pathlocal_.dlb = msg->dlb;
}

void SAARTI::obstacles_callback(const common::Obstacles::ConstPtr& msg){
    obst_.s = msg->s;
    obst_.d = msg->d;
    obst_.R = msg->R;
    obst_.Rmgn = msg->Rmgn;
}

} // end namespace

int main(int argc, char **argv)
{
    ros::init(argc, argv, "motionplanner");
    ros::NodeHandle nh;
    saarti_node::SAARTI saarti(nh);
    return 0;
}
