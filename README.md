# Recordroid: Platform Part
Recordroid is a response-aware replaying tool for Android.

The source code of Recordroid is composed of platform part and controller part.

Please refer to [here](http://github.com/RedCarrottt/recordroid-controller) for accessing other parts.

# How to Build
1. Download whole source code of AOSP(In this case, download AOSP 4.4.2-r2).
  * [Refer to here](https://source.android.com/source/downloading.html)
1. Backup framework/base in AOSP to another place.
  1. $ mv ${AOSP_PATH}/framework/base base-backup
1. Download source code of Recordroid platform part and replace the framework/base with it.
  1. $ git clone https://github.com/RedCarrottt/recordroid
  1. $ mv recordroid ${AOSP_PATH}/framework/base
1. Build the AOSP with the Recordroid-customized framework code.
  * [Refer to here](https://source.android.com/source/building.html)
  
# How to Use
In order to record and replay workload on the device, you have to use [Recordroid Controller](http://github.com/RedCarrottt/recordroid-controller) on your Host PC which have USB connection with target Android device.

# Dependency
Since it is initial version, there are strong dependency on Android's platform version. We will port it to more recent version of Android later.

## Android Version
We tested it on only Android 4.4.2 Release 2(android-4.4.2_r2 branch of AOSP).

## Machine
It is tested on LG Nexus 5 (1st generation) and Samsung Nexus 10. Since Android framework part(the part customized for Recordroid) has no machine-dependency, we guess that other Nexus devices would run the Recordroid, but it is not guaranteed.
