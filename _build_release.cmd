@echo off
set FBT_NO_SYNC=1
call fbt.cmd TARGET_HW=7 DEBUG=0 COMPACT=1 updater_package copro_dist fap_dist
