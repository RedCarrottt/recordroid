/*
* aidl file : frameworks/base/core/java/android/os/IRecordroidService.aidl
* This file contains definitions of functions which are exposed by service 
*/
package android.os;

interface IRecordroidService {
	/** 
	* {@hide}
	*/
	void onViewInputEvent(long startTimeMS);
	void onWebPageLoadEvent_Start(String url);
	void onWebPageLoadEvent_Finish(String url);
	void onActivityLaunchEvent(long responseTimeUS, String componentName);
	void onActivityPauseEvent();
	void onViewShortClickEvent();
	void onViewLongClickEvent();
}
