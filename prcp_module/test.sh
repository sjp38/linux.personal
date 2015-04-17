#!/bin/bash
cd /usr/cpu2006
sleep 5
/usr/cpu2006/single_test.sh 
sleep 5
/usr/cpu2006/different_type.sh 
sleep 5
/usr/cpu2006/same_type.sh 
sleep 5
/usr/cpu2006/cadpm_single_test.sh 
sleep 5
/usr/cpu2006/cadpm_different_type.sh 
sleep 5
/usr/cpu2006/cadpm_same_type.sh
sleep 5
echo "all tests ended.\n"
