#!/usr/bin/env python

'''
Description: This node
    - controls pop-up obstacles
    - publishes state marker (including current mu)
    - all other nodes shut down when this shuts down
'''

import numpy as np
import rospy
from common.msg import Path
from common.msg import Obstacles
from common.msg import State
from common.msg import SaartiStatus
from std_msgs.msg import Int16
from visualization_msgs.msg import Marker
from coordinate_transforms import ptsFrenetToCartesian

class ExperimentManager:
    # constructor
    def __init__(self):
        
        # init node
        rospy.init_node('experiment_manager') 
        
        # timing params
        self.t_activate = rospy.get_param('/t_activate')
        self.dt = 0.01
        self.rate = rospy.Rate(1/self.dt)               
        
        # pop-up scenario params
        self.s_ego_at_popup = rospy.get_param('/s_ego_at_popup')
        self.s_obs_at_popup = rospy.get_param('/s_obs_at_popup')
        self.d_obs_at_popup = rospy.get_param('/d_obs_at_popup')
        
        # track params
        self.track_name = rospy.get_param('/track_name')
        self.s_begin_mu_segments = rospy.get_param('/s_begin_mu_segments')
        self.mu_segment_values = rospy.get_param('/mu_segment_values')
        self.N_mu_segments = len(self.s_begin_mu_segments)
        self.mu_segment_idx = 0
        
        # vehicle params
        self.robot_name = rospy.get_param('/robot_name') 
        self.vehicle_width = rospy.get_param('/car/kinematics/l_width')    
                
        # init subs pubs
        self.pathglobalsub = rospy.Subscriber("pathglobal", Path, self.pathglobal_callback)
        self.statesub = rospy.Subscriber("state", State, self.state_callback)
        self.statussub = rospy.Subscriber("saarti_status", SaartiStatus, self.status_callback)
        self.obspub = rospy.Publisher('/obs', Obstacles, queue_size=1)
        self.obsvispub = rospy.Publisher('/obs_vis', Marker, queue_size=1)
        self.ctrl_mode_pub = rospy.Publisher('/ctrl_mode', Int16, queue_size=1)
        self.statetextmarkerpub = rospy.Publisher('/state_text_marker', Marker, queue_size=1)        
        
        # init misc internal variables
        self.pathglobal = Path()
        self.received_pathglobal = False
        self.state = State()
        self.received_state = False
        self.saarti_status = SaartiStatus()
        self.received_saartistatus = False
        
        while(not self.received_pathglobal):
            rospy.loginfo_throttle(1, "exp_manager: waiting pathglobal")
            self.rate.sleep()

        # init experiment variables
        self.scenario_id = rospy.get_param('/scenario_id')
        self.traction_adaptive  = rospy.get_param('/traction_adaptive')
        
        # init obstacles
        self.obs = Obstacles()
        self.obs.s = [self.s_obs_at_popup]
        self.obs.d = [self.d_obs_at_popup]
        self.obs.R = [0.5]
        wiggleroom = 1.0 # todo param
        self.obs.Rmgn = [0.5*self.obs.R[0] + 0.5*self.vehicle_width + wiggleroom]
        Xobs, Yobs = ptsFrenetToCartesian(np.array(self.obs.s), \
                                          np.array(self.obs.d), \
                                          np.array(self.pathglobal.X), \
                                          np.array(self.pathglobal.Y), \
                                          np.array(self.pathglobal.psi_c), \
                                          np.array(self.pathglobal.s))
        self.ctrl_mode = 0 # # 0: stop, 1: cruise_ctrl, 2: tamp 
        
        # main loop
        self.exptime = 0 
        while (not rospy.is_shutdown()):
            if (self.exptime >= self.t_activate):        
                rospy.loginfo_throttle(1, "Running experiment, ctrl mode = %i"%self.ctrl_mode)
                
                # get current mu for visualization
                s_ego = self.state.s % self.s_lap                
                for i in range(self.N_mu_segments-1):
                    if(self.s_begin_mu_segments[i] <= s_ego <= self.s_begin_mu_segments[i+1]):
                        self.mu_segment_idx = i
                        break
                if(s_ego >= self.s_begin_mu_segments[-1]):
                    self.mu_segment_idx = self.N_mu_segments-1
                mu = self.mu_segment_values[self.mu_segment_idx] 
                
                # POPUP SCENARIO
                if (self.scenario_id in [1,4] ):
                    self.ctrl_mode = 1 # cruise control
                    if(self.state.vx > 5):
                        self.ctrl_mode = 2 # tamp
                    m_obs = self.getobstaclemarker(Xobs,Yobs,self.obs.R[0])
                    m_obs.color.a = 0.3 # transparent before detect
                    if (self.state.s >= self.s_ego_at_popup):
                        self.obspub.publish(self.obs)
                        m_obs.color.a = 1.0 # non-transparent after detect
                    self.obsvispub.publish(m_obs)
                
                # REDUCED MU TURN
                elif(self.scenario_id == 2):
                    self.ctrl_mode = 1 # pp cruise control from standstill
                    if(self.state.vx > 5): # todo
                        self.ctrl_mode = 2 # tamp cruise control when we get up to speed
                                       
                # RACING
                else:
                    self.ctrl_mode = 2 # tamp
                
                # SEND STOP IF EXIT TRACK
                dlb = np.interp(self.state.s,self.pathglobal.s,self.pathglobal.dlb)
                dub = np.interp(self.state.s,self.pathglobal.s,self.pathglobal.dub)
                if (self.state.d < dlb-1.0 or self.state.d > dub+1.0): # todo get from param
                    self.ctrl_mode = 0 # stop
                self.ctrl_mode_pub.publish(self.ctrl_mode)
                
                # publish text marker (state info)
                if(self.traction_adaptive):
                    traction_adaptive_str = "on"
                else:
                    traction_adaptive_str = "off"
                    
                state_text = "traction_adapt: " + traction_adaptive_str + "\n"  \
                             "s:  " + "%.3f" % self.state.s + "\n"  \
                             "vx: " + "%.3f" % self.state.vx + "\n"  \
                             "mu: " + "%.3f" % mu            
                self.statetextmarkerpub.publish(self.gettextmarker(state_text))    
                          
            else: # not reached activation time
                rospy.loginfo_throttle(1, "Experiment starting in %i seconds"%(self.t_activate-self.exptime))
            
            self.exptime += self.dt
            self.rate.sleep()


    def getobstaclemarker(self,X,Y,R):
        m = Marker()
        height =1.75
        m.header.stamp = rospy.Time.now()
        m.header.frame_id = "map"
        m.pose.position.x = X;
        m.pose.position.y = Y;
        m.pose.position.z = height/2.0;
        m.type = m.CYLINDER;
        m.scale.x = R;
        m.scale.y = R;
        m.scale.z = height;
        m.color.a = 1.0; 
        m.color.r = 1.0;
        m.color.g = 0.0;
        m.color.b = 0.0;
        return m

    def gettextmarker(self,text):
        m = Marker()
        m.header.stamp = rospy.Time.now()
        m.header.frame_id = "base_link"
        m.pose.position.x = 0;
        m.pose.position.y = 7.5;
        m.pose.position.z = 5.0;
        m.type = m.TEXT_VIEW_FACING;
        m.text = text
        m.scale.x = 1.0;
        m.scale.y = 1.0;
        m.scale.z = 1.0;
        m.color.a = 1.0; 
        m.color.r = 0.0;
        m.color.g = 0.0;
        m.color.b = 0.0;
        return m

    def pathglobal_callback(self, msg):
        self.pathglobal = msg
        
        # get s of one lap
        stot_global = self.pathglobal.s[-1]
        dist_sf = np.sqrt( (self.pathglobal.X[0]-self.pathglobal.X[-1])**2 + (self.pathglobal.Y[0]-self.pathglobal.Y[-1])**2)
        self.s_lap = stot_global + dist_sf  
        self.received_pathglobal = True
        
    def state_callback(self, msg):
        self.state = msg
        self.received_state = True
    
    def status_callback(self, msg):
        self.saarti_status = msg
        self.received_saartistatus = True
    
    
if __name__ == '__main__':
    em = ExperimentManager()
    try:
        rospy.spin()
    except KeyboardInterrupt:
        print("Shutting down")   
    
    
    
    



    
    
