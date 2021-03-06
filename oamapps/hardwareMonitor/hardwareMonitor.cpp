/* Copyright (C) 2014 InfiniDB, Inc.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
   MA 02110-1301, USA. */

/***************************************************************************
 * $Id: hardwareMonitor.cpp 34 2006-09-29 21:13:54Z dhill $
 *
 *   Author: David Hill
 ***************************************************************************/

#include "hardwareMonitor.h"

using namespace std;
using namespace oam;
using namespace alarmmanager;
using namespace logging;


/************************************************************************************************************
* @brief	main function
*
* purpose:	Get current hardware status and report alarms
*
* Parses file generated by the ipmitool
*
* pattern =  what it is | value | units | status | value 1 | value 2 | value 3 | value 4 | value 5 | value 6
* data(0) = what it is
* data(1) = value
* data(2) = units
* data(3) = status
* data(4)-data(9) = barrier values
*   data(4) - low non-recoverable, i.e. fatal
*   data(5) - low critical
*   data(6) - low warning
*   data(7) - high warning
*   data(8) - high critical
*   data(9) - high non-recoverable, i.e. fatal
*
************************************************************************************************************/
int main (int argc, char** argv)
{
	string data[10];
	string SensorName;
	float SensorValue;
    string Units;
	string SensorStatus;
	float lowFatal;
	float lowCritical;
	float lowWarning;
	float highWarning;
	float highCritical;
	float highFatal;
	char *p;

    // check for IPMI_SUPPORT FLAG passed in
    if(argc > 1)
		IPMI_SUPPORT = atoi(argv[1]);

	// loop forever reading the hardware status
	while(true)
	{
		if( IPMI_SUPPORT == 0) {
			int returnCode = system("ipmitool sensor list > /tmp/harwareMonitor.txt");
			if (returnCode) {
				// System error, Log this event 
				LoggingID lid;
				MessageLog ml(lid);
				Message msg;
				Message::Args args;
				args.add("Error running ipmitool sensor list!!!");
				msg.format(args);
				ml.logWarningMessage(msg);
				sleep(300);
				continue;
			}
		}
	
		// parse output file
	
		ifstream File ("/tmp/harwareMonitor.txt");
		if (!File){
			// System error, Log this event 
			LoggingID lid;
			MessageLog ml(lid);
			Message msg;
			Message::Args args;
			args.add("Error opening /tmp/harwareMonitor.txt!!!");
			msg.format(args);
			ml.logWarningMessage(msg);
			sleep(300);
			continue;
		}
		
		char line[200];
		while (File.getline(line, 200))
		{
			// parse the line
			int f = 0;
			p = strtok(line,"|");
			while (p) 
			{
				data[f]=p;
				data[f] = StripWhitespace(data[f]);
				p = strtok (NULL, "|");
				f++;
			}
	
			if( f == 0 )
				// nothing on this line, skip
				continue;
	
			SensorName = data[0];
			SensorValue = atof(data[1].c_str());
			Units = data[2];
			SensorStatus = data[3];
			lowFatal = atof(data[4].c_str());
			lowCritical = atof(data[5].c_str());
			lowWarning = atof(data[6].c_str());
			highWarning = atof(data[7].c_str());
			highCritical = atof(data[8].c_str());
			highFatal = atof(data[9].c_str());

			// check status and issue apporiate alarm if needed
			if ( (SensorStatus != "ok") && (SensorStatus != "nr") && (SensorStatus != "na") ) {
				// Status error, check for warning or critical levels

				if ( SensorValue >= highFatal ) {
					// issue critical alarm and send message to shutdown Server
					sendAlarm(SensorName, HARDWARE_HIGH, SET, SensorValue);
					sendMsgShutdownServer();
				}
				else if ( (SensorValue < highFatal) && (SensorValue >= highCritical) )
					// issue major alarm
					sendAlarm(SensorName, HARDWARE_MED, SET, SensorValue);

				else if ( (SensorValue < highCritical ) && (SensorValue >= highWarning) )
					// issue minor alarm
					sendAlarm(SensorName, HARDWARE_LOW, SET, SensorValue);

				else if ( (SensorValue <= lowWarning) && (SensorValue > lowCritical) )
					// issue minor alarm
					sendAlarm(SensorName, HARDWARE_LOW, SET, SensorValue);

				else if ( (SensorValue <= lowCritical) && (SensorValue > lowFatal) )
					// issue major alarm
					sendAlarm(SensorName, HARDWARE_MED, SET, SensorValue);

				else if ( SensorValue <= lowFatal ) {
					// issue critical alarm and send message to shutdown Server
					sendAlarm(SensorName, HARDWARE_HIGH, SET, SensorValue);
					sendMsgShutdownServer();
				}
				else
					// check if there are any active alarms that needs to be cleared
					checkAlarm(SensorName);
			}
			else
				// check if there are any active alarms that needs to be cleared
				checkAlarm(SensorName);

		} //end of parsing file while
		
		File.close();
		// sleep for 1 minute
		sleep(60);
	} //end of forever while loop
}
	
/******************************************************************************************
* @brief	sendAlarm
*
* purpose:	send a trap and log the process information
*
******************************************************************************************/
void sendAlarm(string alarmItem, ALARMS alarmID, int action, float sensorValue)
{
	Oam oam;

	//Log this event 
	LoggingID lid;
	MessageLog ml(lid);
	Message msg;
	Message::Args args;
	args.add(alarmItem);
	args.add(", sensor value out-of-range: ");
	args.add(sensorValue);

	// get current server name
	string serverName;
	oamServerInfo_t st;
	try {
		st = oam.getServerInfo();
		serverName = boost::get<0>(st);
	}
	catch (...) {
		serverName = "Unknown Server";
	}

	// check if there is an active alarm above the reporting theshold 
	// that needs to be cleared
	checkAlarm(alarmItem, alarmID);

	// check if Alarm is already active, don't resend
	if ( !( oam.checkActiveAlarm(alarmID, serverName, alarmItem)) ) {

		ALARMManager alarmMgr;
		// send alarm
		alarmMgr.sendAlarmReport(alarmItem.c_str(), alarmID, action);

		args.add(", Alarm set: ");
		args.add(alarmID);
	}

	// output log
	msg.format(args);
	ml.logWarningMessage(msg);

	return;
}

/******************************************************************************************
* @brief	checkAlarm
*
* purpose:	check to see if an alarm(s) is set on device and clear if so
*
******************************************************************************************/
void checkAlarm(string alarmItem, ALARMS alarmID)
{
	Oam oam;

	// get current server name
	string serverName;
	oamServerInfo_t st;
	try {
		st = oam.getServerInfo();
		serverName = boost::get<0>(st);
	}
	catch (...) {
		serverName = "Unknown Server";
	}

	switch (alarmID) {
		case ALARM_NONE: 	// clear all alarms set if any found
			if ( oam.checkActiveAlarm(HARDWARE_HIGH, serverName, alarmItem) )
				//  alarm set, clear it
				clearAlarm(alarmItem, HARDWARE_HIGH);
			if ( oam.checkActiveAlarm(HARDWARE_MED, serverName, alarmItem) )
				//  alarm set, clear it
				clearAlarm(alarmItem, HARDWARE_MED);
			if ( oam.checkActiveAlarm(HARDWARE_LOW, serverName, alarmItem) )
				//  alarm set, clear it
				clearAlarm(alarmItem, HARDWARE_LOW);
			break;
		case HARDWARE_LOW: 	// clear high and medium alarms set if any found
			if ( oam.checkActiveAlarm(HARDWARE_HIGH, serverName, alarmItem) )
				//  alarm set, clear it
				clearAlarm(alarmItem, HARDWARE_HIGH);
			if ( oam.checkActiveAlarm(HARDWARE_MED, serverName, alarmItem) )
				//  alarm set, clear it
				clearAlarm(alarmItem, HARDWARE_MED);
			break;
		case HARDWARE_MED: 	// clear high alarms set if any found
			if ( oam.checkActiveAlarm(HARDWARE_HIGH, serverName, alarmItem) )
				//  alarm set, clear it
				clearAlarm(alarmItem, HARDWARE_HIGH);
			break;
		default:			// none to clear
			break;
		} // end of switch
	return;
}

/******************************************************************************************
* @brief	clearAlarm
*
* purpose:	clear Alarm that was previously set
*
******************************************************************************************/
void clearAlarm(string alarmItem, ALARMS alarmID)
{
	ALARMManager alarmMgr;
	alarmMgr.sendAlarmReport(alarmItem.c_str(), alarmID, CLEAR);

	//Log this event 
	LoggingID lid;
	MessageLog ml(lid);
	Message msg;
	Message::Args args;
	args.add(alarmItem);
	args.add(" alarm #");
	args.add(alarmID);
	args.add("cleared");
	msg.format(args);
	ml.logWarningMessage(msg);
}
/******************************************************************************************
* @brief	sendMsgShutdownServer
*
* purpose:	send a Message to Shutdown server
*
******************************************************************************************/
void sendMsgShutdownServer()
{
	Oam oam;

	//Log this event 
	LoggingID lid;
	MessageLog ml(lid);
	Message msg;
	Message::Args args;
	args.add("Fatal Hardware Alarm detected, Server being shutdown");
	msg.format(args);
	ml.logCriticalMessage(msg);

	string serverName;
	oamServerInfo_t st;
	try {
		st = oam.getServerInfo();
		serverName = boost::get<0>(st);
	}
	catch (...) {
		// o well, let's take out own action
		if( IPMI_SUPPORT == 0)
			system("init 0");
	}

	try
	{
		oam.shutdownServer(serverName, FORCEFUL, ACK_NO);
	}
	catch (exception& e)
	{
		// o well, let's take out own action
		if( IPMI_SUPPORT == 0)
			system("init 0");
	}
}

/******************************************************************************************
* @brief	StripWhitespace
*
* purpose:	strip off whitespaces from a string
*
******************************************************************************************/
string StripWhitespace(string value)
{
	for(;;)
	{
		string::size_type pos = value.find (' ',0);
		if (pos == string::npos)
			// no more found
			break;
		// strip leading
		if (pos == 0) {
			value = value.substr (pos+1,10000);
		}
		else 
		{ // strip trailing
			value = value.substr (0, pos);
		}
	}
	return value;
}
