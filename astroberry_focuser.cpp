/*******************************************************************************
  Copyright(c) 2014-2020 Radek Kaczorek  <rkaczorek AT gmail DOT com>

 This library is free software; you can redistribute it and/or
 modify it under the terms of the GNU Library General Public
 License version 2 as published by the Free Software Foundation.
 .
 This library is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 Library General Public License for more details.
 .
 You should have received a copy of the GNU Library General Public License
 along with this library; see the file COPYING.LIB.  If not, write to
 the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 Boston, MA 02110-1301, USA.
*******************************************************************************/

/*
 * TO DO:
 * - Save position in xml instead flat file
 * - Add temperature compensation auto learning and save temperature compensation curve to xml
 * - Add simulation mode
 */

#include <stdio.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <fstream>
#include <math.h>
#include <memory>
#include "config.h"
#include "indicontroller.h"

#include <gpiod.h>

#include "astroberry_focuser.h"

// We declare an auto pointer to AstroberryFocuser.
std::unique_ptr<AstroberryFocuser> astroberryFocuser(new AstroberryFocuser());

#define MAX_RESOLUTION 2 // the highest resolution supported is 1/2 step
#define TEMPERATURE_UPDATE_TIMEOUT (60 * 1000) // 60 sec
#define STEPPER_STANDBY_TIMEOUT (60 * 1000) // 60 sec
#define TEMPERATURE_COMPENSATION_TIMEOUT (60 * 1000) // 60 sec

int halfStep[8][4] = { {1,0,0,0}, {1,1,0,0}, {0,1,0,0}, {0,1,1,0}, {0,0,1,0}, {0,0,1,1}, {0,0,0,1}, {1,0,0,1} };
int fullStep[8][4] = { {1,0,0,0}, {0,1,0,0}, {0,0,1,0}, {0,0,0,1}, {1,0,0,0}, {0,1,0,0}, {0,0,1,0}, {0,0,0,1} };

void ISPoll(void *p);


void ISInit()
{
	static int isInit = 0;

	if (isInit == 1)
	return;
	if(astroberryFocuser.get() == 0)
	{
	isInit = 1;
	astroberryFocuser.reset(new AstroberryFocuser());
	}
}

void ISGetProperties(const char *dev)
{
        ISInit();
        astroberryFocuser->ISGetProperties(dev);
}

void ISNewSwitch(const char *dev, const char *name, ISState *states, char *names[], int num)
{
        ISInit();
        astroberryFocuser->ISNewSwitch(dev, name, states, names, num);
}

void ISNewText(	const char *dev, const char *name, char *texts[], char *names[], int num)
{
        ISInit();
        astroberryFocuser->ISNewText(dev, name, texts, names, num);
}

void ISNewNumber(const char *dev, const char *name, double values[], char *names[], int num)
{
        ISInit();
        astroberryFocuser->ISNewNumber(dev, name, values, names, num);
}

void ISNewBLOB (const char *dev, const char *name, int sizes[], int blobsizes[], char *blobs[], char *formats[], char *names[], int n)
{
	INDI_UNUSED(dev);
	INDI_UNUSED(name);
	INDI_UNUSED(sizes);
	INDI_UNUSED(blobsizes);
	INDI_UNUSED(blobs);
	INDI_UNUSED(formats);
	INDI_UNUSED(names);
	INDI_UNUSED(n);
}

void ISSnoopDevice (XMLEle *root)
{
	astroberryFocuser->ISSnoopDevice(root);
}

AstroberryFocuser::AstroberryFocuser()
{
	setVersion(VERSION_MAJOR,VERSION_MINOR);
	FI::SetCapability(FOCUSER_CAN_ABS_MOVE | FOCUSER_CAN_REL_MOVE | FOCUSER_CAN_REVERSE | FOCUSER_CAN_SYNC | FOCUSER_CAN_ABORT);
	Focuser::setSupportedConnections(CONNECTION_NONE);
}

AstroberryFocuser::~AstroberryFocuser()
{
	// delete properties independent of connection status
	deleteProperty(MotorBoardSP.name);
	deleteProperty(BCMpinsNP.name);
}

const char * AstroberryFocuser::getDefaultName()
{
        return (char *)"Astroberry Focuser";
}

bool AstroberryFocuser::Connect()
{
	chip = gpiod_chip_open("/dev/gpiochip0");
	if (!chip)
	{
		DEBUG(INDI::Logger::DBG_ERROR, "Problem initiating Astroberry Focuser.");
		return false;
	}

	// verify BCM Pins are not used by other consumers
	for (unsigned int pin = 0; pin < 4 ; pin++)
	{
		if (gpiod_line_is_used(gpiod_chip_get_line(chip, BCMpinsN[pin].value)))
		{
			DEBUGF(INDI::Logger::DBG_ERROR, "BCM Pin %0.0f already used", BCMpinsN[pin].value);
			gpiod_chip_close(chip);
			return false;
		}
	}

	// Select gpios
	gpio_in1 = gpiod_chip_get_line(chip, BCMpinsN[0].value);
	gpio_in2 = gpiod_chip_get_line(chip, BCMpinsN[1].value);
	gpio_in3 = gpiod_chip_get_line(chip, BCMpinsN[2].value);
	gpio_in4 = gpiod_chip_get_line(chip, BCMpinsN[3].value);

	// Set initial state for gpios
	gpiod_line_request_output(gpio_in1, "in1@astroberry_focuser", 0);
	gpiod_line_request_output(gpio_in2, "in2@astroberry_focuser", 0);
	gpiod_line_request_output(gpio_in3, "in3@astroberry_focuser", 0);
	gpiod_line_request_output(gpio_in4, "in4@astroberry_focuser", 0);

	//read last position from file & convert from MAX_RESOLUTION to current resolution
	FocusAbsPosN[0].value = savePosition(-1) != -1 ? (int) savePosition(-1) * resolution / MAX_RESOLUTION : 0;

	// preset resolution
	SetResolution(resolution);

	// Lock Motor Board setting
	MotorBoardSP.s=IPS_BUSY;
	IDSetSwitch(&MotorBoardSP, nullptr);

	// Lock BCM Pins setting
	BCMpinsNP.s=IPS_BUSY;
	IDSetNumber(&BCMpinsNP, nullptr);

	// Update focuser parameters
	getFocuserInfo();

	// set motor standby timer
	stepperStandbyID = IEAddTimer(STEPPER_STANDBY_TIMEOUT, stepperStandbyHelper, this);

	DEBUG(INDI::Logger::DBG_SESSION, "Astroberry Focuser connected successfully.");

	return true;
}

bool AstroberryFocuser::Disconnect()
{
	// Close device
	gpiod_chip_close(chip);

	// Unlock Motor Board setting
	MotorBoardSP.s=IPS_IDLE;
	IDSetSwitch(&MotorBoardSP, nullptr);

	// Unlock BCM Pins setting
	BCMpinsNP.s=IPS_IDLE;
	IDSetNumber(&BCMpinsNP, nullptr);

	// Stop timers
	IERmTimer(stepperStandbyID);
	IERmTimer(updateTemperatureID);
	IERmTimer(temperatureCompensationID);

	DEBUG(INDI::Logger::DBG_SESSION, "Astroberry Focuser disconnected successfully.");

	return true;
}

bool AstroberryFocuser::initProperties()
{
	INDI::Focuser::initProperties();

	// Focuser Resolution
	IUFillSwitch(&FocusResolutionS[0],"FOCUS_RESOLUTION_1","Full Step",ISS_OFF);
	IUFillSwitch(&FocusResolutionS[1],"FOCUS_RESOLUTION_2","Half Step",ISS_ON);
	IUFillSwitchVector(&FocusResolutionSP,FocusResolutionS,2,getDeviceName(),"FOCUS_RESOLUTION","Resolution",MAIN_CONTROL_TAB,IP_RW,ISR_1OFMANY,0,IPS_IDLE);

	//Change Focuser Step Size
	IUFillSwitch(&FocusStepChangeS[0], "FOCUS_STEPS_DOWN", "Focus Steps Down", ISS_ON);
	IUFillSwitch(&FocusStepChangeS[1], "FOCUS_STEPS_UP", "Focus Step Up", ISS_OFF);
	IUFillSwitchVector(&FocusStepChangeSP, FocusStepChangeS, 2, m_defaultDevice->getDeviceName(), "FOCUS_STEPS_CHANGE", "Change Steps", MAIN_CONTROL_TAB, IP_RW, ISR_1OFMANY, 0, IPS_IDLE);

	// Focuser Info
	IUFillNumber(&FocuserInfoN[0], "CFZ_STEP_ACT", "Step Size (μm)", "%0.2f", 0, 1000, 1, 0);
	IUFillNumber(&FocuserInfoN[1], "CFZ", "Critical Focus Zone (μm)", "%0.2f", 0, 1000, 1, 0);
	IUFillNumber(&FocuserInfoN[2], "STEPS_PER_CFZ", "Steps / Critical Focus Zone", "%0.0f", 0, 1000, 1, 0);
	IUFillNumberVector(&FocuserInfoNP, FocuserInfoN, 3, getDeviceName(), "FOCUSER_PARAMETERS", "Focuser Info", MAIN_CONTROL_TAB, IP_RO, 0, IPS_IDLE);

	// Focuser Stepper Controller
	IUFillSwitch(&MotorBoardS[0],"ULN2003","ULN2003",ISS_ON);
	IUFillSwitchVector(&MotorBoardSP,MotorBoardS,1,getDeviceName(),"MOTOR_BOARD","Control Board",OPTIONS_TAB,IP_RW,ISR_1OFMANY,0,IPS_IDLE);

	// BCM PINs setting
	IUFillNumber(&BCMpinsN[0], "BCMPIN_IN1", "IN1", "%0.0f", 1, 27, 0, 17); // BCM17 = PIN12
	IUFillNumber(&BCMpinsN[1], "BCMPIN_IN2", "IN2", "%0.0f", 1, 27, 0, 18); // BCM18 = PIN11
	IUFillNumber(&BCMpinsN[2], "BCMPIN_IN3", "IN3", "%0.0f", 1, 27, 0, 27); // BCM27 = PIN13
	IUFillNumber(&BCMpinsN[3], "BCMPIN_IN4", "IN4", "%0.0f", 1, 27, 0, 22); // BCM22 = PIN15
	IUFillNumberVector(&BCMpinsNP, BCMpinsN, 4, getDeviceName(), "BCMPINS", "BCM Pins", OPTIONS_TAB, IP_RW, 0, IPS_IDLE);

	// Step delay setting
	IUFillNumber(&FocusStepDelayN[0], "FOCUS_STEPDELAY_VALUE", "milliseconds", "%0.0f", 1, 1000, 1, 20.0);
	IUFillNumberVector(&FocusStepDelayNP, FocusStepDelayN, 1, getDeviceName(), "FOCUS_STEPDELAY", "Step Delay", OPTIONS_TAB, IP_RW, 0, IPS_IDLE);

	// Backlash setting
	IUFillNumber(&FocusBacklashN[0], "FOCUS_BACKLASH_VALUE", "steps", "%0.0f", 0, 1000, 10, 0);
	IUFillNumberVector(&FocusBacklashNP, FocusBacklashN, 1, getDeviceName(), "FOCUS_BACKLASH", "Backlash", OPTIONS_TAB, IP_RW, 0, IPS_IDLE);

	// Reset absolute possition
	IUFillSwitch(&ResetAbsPosS[0],"RESET_ABS","Purge",ISS_OFF);
	IUFillSwitchVector(&ResetAbsPosSP,ResetAbsPosS,1,getDeviceName(),"RESET_ABS_SW","Saved Position",OPTIONS_TAB,IP_RW,ISR_1OFMANY,0,IPS_IDLE);

	// Active telescope setting
	IUFillText(&ActiveTelescopeT[0], "ACTIVE_TELESCOPE_NAME", "Telescope", "Telescope Simulator");
	IUFillTextVector(&ActiveTelescopeTP, ActiveTelescopeT, 1, getDeviceName(), "ACTIVE_TELESCOPE", "Snoop devices", OPTIONS_TAB,IP_RW, 0, IPS_IDLE);

	// Maximum focuser travel
	IUFillNumber(&FocuserTravelN[0], "FOCUSER_TRAVEL_VALUE", "mm", "%0.0f", 10, 200, 10, 10);
	IUFillNumberVector(&FocuserTravelNP, FocuserTravelN, 1, getDeviceName(), "FOCUSER_TRAVEL", "Max Travel", OPTIONS_TAB, IP_RW, 0, IPS_IDLE);

	// Focuser temperature
	IUFillNumber(&FocusTemperatureN[0], "FOCUS_TEMPERATURE_VALUE", "°C", "%0.2f", -50, 50, 1, 0);
	IUFillNumberVector(&FocusTemperatureNP, FocusTemperatureN, 1, getDeviceName(), "FOCUS_TEMPERATURE", "Temperature", MAIN_CONTROL_TAB, IP_RO, 0, IPS_IDLE);

	// Temperature Coefficient
	IUFillNumber(&TemperatureCoefN[0], "μm/m°C", "", "%.1f", 0, 50, 1, 0);
	IUFillNumberVector(&TemperatureCoefNP, TemperatureCoefN, 1, getDeviceName(), "Temperature Coefficient", "", MAIN_CONTROL_TAB, IP_RW, 0, IPS_IDLE);

	// Compensate for temperature
	IUFillSwitch(&TemperatureCompensateS[0], "Enable", "", ISS_OFF);
	IUFillSwitch(&TemperatureCompensateS[1], "Disable", "", ISS_ON);
	IUFillSwitchVector(&TemperatureCompensateSP, TemperatureCompensateS, 2, getDeviceName(), "Temperature Compensate", "", MAIN_CONTROL_TAB, IP_RW, ISR_1OFMANY, 0, IPS_IDLE);

	// Snooping params
	IUFillNumber(&ScopeParametersN[0], "TELESCOPE_APERTURE", "Aperture (mm)", "%g", 10, 5000, 0, 150.0);
	IUFillNumber(&ScopeParametersN[1], "TELESCOPE_FOCAL_LENGTH", "Focal Length (mm)", "%g", 10, 10000, 0, 1500.0);
	IUFillNumberVector(&ScopeParametersNP, ScopeParametersN, 2, ActiveTelescopeT[0].text, "TELESCOPE_INFO", "Scope Properties", OPTIONS_TAB, IP_RW, 60, IPS_OK);

	// initial values at resolution 1/1
	FocusMaxPosN[0].min = 1000;
	FocusMaxPosN[0].max = 100000;
	FocusMaxPosN[0].step = 1000;
	FocusMaxPosN[0].value = 10000;

	FocusRelPosN[0].min = 0;
	FocusRelPosN[0].max = 1000;
	FocusRelPosN[0].step = 10;
	FocusRelPosN[0].value = 10;

	FocusAbsPosN[0].min = 0;
	FocusAbsPosN[0].max = FocusMaxPosN[0].value;
	FocusAbsPosN[0].step = (int) FocusAbsPosN[0].max / 100;

	FocusMotionS[FOCUS_OUTWARD].s = ISS_ON;
	FocusMotionS[FOCUS_INWARD].s = ISS_OFF;
	IDSetSwitch(&FocusMotionSP, nullptr);

	// Add default properties
	// addAuxControls(); // use instead if simulation mode is added to code
	addDebugControl ();
	addConfigurationControl();
	removeProperty("POLLING_PERIOD", nullptr);

	// Load some custom properties before connecting
	defineSwitch(&MotorBoardSP);
	defineNumber(&BCMpinsNP);

	// Load config values, which cannot be changed after we are connected
	loadConfig(false, "MOTOR_BOARD"); // load stepper motor controller
	loadConfig(false, "BCMPINS"); // load BCM Pins assignment

	return true;
}

void AstroberryFocuser::ISGetProperties (const char *dev)
{
	INDI::Focuser::ISGetProperties(dev);
	return;
}

bool AstroberryFocuser::updateProperties()
{
	INDI::Focuser::updateProperties();

	if (isConnected())
	{
		defineText(&ActiveTelescopeTP);
		defineNumber(&FocuserTravelNP);
		defineSwitch(&FocusMotionSP);
		defineSwitch(&FocusResolutionSP);
		defineSwitch(&FocusStepChangeSP);
		defineNumber(&FocuserInfoNP);
		defineNumber(&FocusStepDelayNP);
		defineNumber(&FocusBacklashNP);
		defineSwitch(&ResetAbsPosSP);

		IDSnoopDevice(ActiveTelescopeT[0].text, "TELESCOPE_INFO");

		if (readDS18B20())
		{
			defineNumber(&FocusTemperatureNP);
			defineNumber(&TemperatureCoefNP);
			defineSwitch(&TemperatureCompensateSP);
			readDS18B20(); // update immediately
			lastTemperature = FocusTemperatureN[0].value; // init last temperature
			IERmTimer(updateTemperatureID);
			updateTemperatureID = IEAddTimer(TEMPERATURE_UPDATE_TIMEOUT, updateTemperatureHelper, this); // set temperature update timer
			IERmTimer(temperatureCompensationID);
			temperatureCompensationID = IEAddTimer(TEMPERATURE_COMPENSATION_TIMEOUT, temperatureCompensationHelper, this); // set temperature compensation timer
		}

	} else {
		deleteProperty(ActiveTelescopeTP.name);
		deleteProperty(FocuserTravelNP.name);
		deleteProperty(FocusMotionSP.name);
		deleteProperty(FocusResolutionSP.name);
                deleteProperty(FocusStepChangeSP.name);
		deleteProperty(FocuserInfoNP.name);
		deleteProperty(FocusStepDelayNP.name);
		deleteProperty(FocusBacklashNP.name);
		deleteProperty(ResetAbsPosSP.name);
		deleteProperty(FocusTemperatureNP.name);
		deleteProperty(TemperatureCoefNP.name);
		deleteProperty(TemperatureCompensateSP.name);
	}

	return true;
}

bool AstroberryFocuser::ISNewNumber (const char *dev, const char *name, double values[], char *names[], int n)
{
	// first we check if it's for our device
	if(!strcmp(dev,getDeviceName()))
	{
	        // handle BCMpins
	        if (!strcmp(name, BCMpinsNP.name))
	        {
			unsigned int valcount = 4;

			if (isConnected())
			{
				DEBUG(INDI::Logger::DBG_WARNING, "Cannot set BCM Pins while device is connected.");
				return false;
			} else {
				for (unsigned int i = 0; i < valcount; i++)
				{
					// verify a number is a valid BCM Pin
					if ( values[i] < 1 || values[i] > 27 )
					{
						BCMpinsNP.s=IPS_ALERT;
						IDSetNumber(&BCMpinsNP, nullptr);
						DEBUGF(INDI::Logger::DBG_ERROR, "Value %0.0f is not a valid BCM Pin number!", values[i]);
						return false;
					}

					// Verify unique BCM Pin assignement
					for (unsigned j = i + 1; j < valcount; j++)
					{
						if ( values[i] == values[j] )
						{
							BCMpinsNP.s=IPS_ALERT;
							IDSetNumber(&BCMpinsNP, nullptr);
							DEBUG(INDI::Logger::DBG_ERROR, "You cannot assign the same BCM Pin twice!");
							return false;
						}
					}

					// verify BCM Pins are not used by other consumers
					chip = gpiod_chip_open("/dev/gpiochip0");
					if (chip)
					{
						struct gpiod_line *line = gpiod_chip_get_line(chip, values[i]);
						bool line_status = gpiod_line_is_used(line);
						gpiod_chip_close(chip);

						if (line_status)
						{
							BCMpinsNP.s=IPS_ALERT;
							IDSetNumber(&BCMpinsNP, nullptr);
							DEBUGF(INDI::Logger::DBG_ERROR, "BCM Pin %0.0f already used!", values[i]);
							return false;
						}
					} else {
						DEBUG(INDI::Logger::DBG_ERROR, "Problem initiating Astroberry Focuser.");
						return false;
					}
				}

				IUUpdateNumber(&BCMpinsNP,values,names,n);

				BCMpinsNP.s=IPS_OK;
				IDSetNumber(&BCMpinsNP, nullptr);
				DEBUGF(INDI::Logger::DBG_SESSION, "BCM Pins set to IN1: BCM%0.0f, IN2: BCM%0.0f, IN3: BCM%0.0f, IN4: BCM%0.0f", BCMpinsN[0].value, BCMpinsN[1].value, BCMpinsN[2].value, BCMpinsN[3].value);
				return true;
			}
	        }

	        // handle focus maximum position
	        if (!strcmp(name, FocusMaxPosNP.name))
	        {
			IUUpdateNumber(&FocusMaxPosNP,values,names,n);

			FocusAbsPosN[0].max = FocusMaxPosN[0].value;
			IUUpdateMinMax(&FocusAbsPosNP); // This call is not INDI protocol compliant

			FocusAbsPosNP.s=IPS_OK;
			IDSetNumber(&FocusMaxPosNP, nullptr);
			getFocuserInfo();
			return true;
	        }

	        // handle focus absolute position
	        if (!strcmp(name, FocusAbsPosNP.name))
	        {
			int newPos = (int) values[0];

			if ( MoveAbsFocuser(newPos) == IPS_OK )
			{
				IUUpdateNumber(&FocusAbsPosNP,values,names,n);
				FocusAbsPosNP.s=IPS_OK;
				IDSetNumber(&FocusAbsPosNP, nullptr);
				return true;
			} else {
				return false;
			}
	        }

	        // handle focus relative position
	        if (!strcmp(name, FocusRelPosNP.name))
	        {
			IUUpdateNumber(&FocusRelPosNP,values,names,n);

			//FOCUS_INWARD
			if ( FocusMotionS[0].s == ISS_ON )
				FocusRelPosNP.s = MoveRelFocuser(FOCUS_INWARD, FocusRelPosN[0].value);

			//FOCUS_OUTWARD
			if ( FocusMotionS[1].s == ISS_ON )
				FocusRelPosNP.s = MoveRelFocuser(FOCUS_OUTWARD, FocusRelPosN[0].value);

			IDSetNumber(&FocusRelPosNP, nullptr);
			return true;
	        }

	        // handle focus backlash
	        if (!strcmp(name, FocusBacklashNP.name))
	        {
			IUUpdateNumber(&FocusBacklashNP,values,names,n);
			FocusBacklashNP.s=IPS_BUSY;
			IDSetNumber(&FocusBacklashNP, nullptr);
			FocusBacklashNP.s=IPS_OK;
			IDSetNumber(&FocusBacklashNP, nullptr);
			DEBUGF(INDI::Logger::DBG_SESSION, "Backlash set to %0.0f steps.", FocusBacklashN[0].value);
			return true;
	        }

	        // handle focus step delay
	        if (!strcmp(name, FocusStepDelayNP.name))
	        {
			IUUpdateNumber(&FocusStepDelayNP,values,names,n);
			FocusStepDelayNP.s=IPS_BUSY;
			IDSetNumber(&FocusStepDelayNP, nullptr);
			FocusStepDelayNP.s=IPS_OK;
			IDSetNumber(&FocusStepDelayNP, nullptr);
			DEBUGF(INDI::Logger::DBG_SESSION, "Step delay set to %0.0f ms.", FocusStepDelayN[0].value);
			return true;
	        }

	        // handle focuser travel
	        if (!strcmp(name, FocuserTravelNP.name))
	        {
			IUUpdateNumber(&FocuserTravelNP,values,names,n);
			getFocuserInfo();
			FocuserTravelNP.s=IPS_OK;
			IDSetNumber(&FocuserTravelNP, nullptr);
			DEBUGF(INDI::Logger::DBG_SESSION, "Maximum focuser travel set to %0.0f mm", FocuserTravelN[0].value);
			return true;
	        }

	        // handle temperature coefficient
	        if (!strcmp(name, TemperatureCoefNP.name))
	        {
			IUUpdateNumber(&TemperatureCoefNP,values,names,n);
			TemperatureCoefNP.s=IPS_OK;
			IDSetNumber(&TemperatureCoefNP, nullptr);
			DEBUGF(INDI::Logger::DBG_SESSION, "Temperature coefficient set to %0.1f μm/m°C", TemperatureCoefN[0].value);
			return true;
	        }
	}

	return INDI::Focuser::ISNewNumber(dev,name,values,names,n);
}

bool AstroberryFocuser::ISNewSwitch (const char *dev, const char *name, ISState *states, char *names[], int n)
{
	// first we check if it's for our device
	if (!strcmp(dev, getDeviceName()))
	{
	        // handle focus presets
		if (!strcmp(name, PresetGotoSP.name))
	        {
			IUUpdateSwitch(&PresetGotoSP, states, names, n);
			PresetGotoSP.s = IPS_BUSY;
			IDSetSwitch(&PresetGotoSP, nullptr);

			//Preset 1
			if ( PresetGotoS[0].s == ISS_ON )
				MoveAbsFocuser(PresetN[0].value);

			//Preset 2
			if ( PresetGotoS[1].s == ISS_ON )
				MoveAbsFocuser(PresetN[1].value);

			//Preset 3
			if ( PresetGotoS[2].s == ISS_ON )
				MoveAbsFocuser(PresetN[2].value);

			PresetGotoS[0].s = ISS_OFF;
			PresetGotoS[1].s = ISS_OFF;
			PresetGotoS[2].s = ISS_OFF;
			PresetGotoSP.s = IPS_OK;

			IDSetSwitch(&PresetGotoSP, nullptr);

			return true;
	        }

	        // handle motor board
	        if(!strcmp(name, MotorBoardSP.name))
	        {
			int current_switch = IUFindOnSwitchIndex(&MotorBoardSP);

			if (isConnected())
			{
				// reset switch to previous state
				MotorBoardS[current_switch].s = ISS_ON;
				IDSetSwitch(&MotorBoardSP, nullptr);
				DEBUG(INDI::Logger::DBG_WARNING, "Cannot set Control Board while device is connected.");
				return false;
			} else {
				IUUpdateSwitch(&MotorBoardSP, states, names, n);

				if ( MotorBoardS[0].s == ISS_ON)
				{
					DEBUG(INDI::Logger::DBG_SESSION, "Control Board set to ULN2003.");
				}

				MotorBoardSP.s = IPS_OK;
				IDSetSwitch(&MotorBoardSP, nullptr);
				return true;
			}
		}

	        // handle focus resolution
	        if(!strcmp(name, FocusResolutionSP.name))
	        {
			int last_resolution = resolution;

			IUUpdateSwitch(&FocusResolutionSP, states, names, n);

			//Resolution 1/1
			if ( FocusResolutionS[0].s == ISS_ON )
				resolution = 1;

			//Resolution 1/2
			if ( FocusResolutionS[1].s == ISS_ON )
				resolution = 2;

			// Adjust position to a step in lower resolution
			int position_adjustment = last_resolution * (FocusAbsPosN[0].value / last_resolution - (int) FocusAbsPosN[0].value / last_resolution);
			if ( resolution < last_resolution && position_adjustment > 0 )
			{
				if ( (float) position_adjustment / last_resolution < 0.5)
				{
					position_adjustment *= -1;
				} else {
					position_adjustment = last_resolution - position_adjustment;
				}
				DEBUGF(INDI::Logger::DBG_SESSION, "Focuser position adjusted by %d steps at 1/%d resolution to sync with 1/%d resolution.", position_adjustment, last_resolution, resolution);
				MoveAbsFocuser(FocusAbsPosN[0].value + position_adjustment);
			}

			SetResolution(resolution);

			// update values based on resolution
			FocusRelPosN[0].min = (int) FocusRelPosN[0].min * resolution / last_resolution;
			FocusRelPosN[0].max = (int) FocusRelPosN[0].max * resolution / last_resolution;
			FocusRelPosN[0].step = (int) FocusRelPosN[0].step * resolution / last_resolution;
			FocusRelPosN[0].value = (int) FocusRelPosN[0].value * resolution / last_resolution;
			IDSetNumber(&FocusRelPosNP, nullptr);
			IUUpdateMinMax(&FocusRelPosNP);

			FocusAbsPosN[0].max = (int) FocusAbsPosN[0].max * resolution / last_resolution;
			FocusAbsPosN[0].step = (int) FocusAbsPosN[0].step * resolution / last_resolution;
			FocusAbsPosN[0].value = (int) FocusAbsPosN[0].value * resolution / last_resolution;
			IDSetNumber(&FocusAbsPosNP, nullptr);
			IUUpdateMinMax(&FocusAbsPosNP);

			FocusMaxPosN[0].min = (int) FocusMaxPosN[0].min * resolution / last_resolution;
			FocusMaxPosN[0].max = (int) FocusMaxPosN[0].max * resolution / last_resolution;
			FocusMaxPosN[0].step = (int) FocusMaxPosN[0].step * resolution / last_resolution;
			FocusMaxPosN[0].value = (int) FocusMaxPosN[0].value * resolution / last_resolution;
			IDSetNumber(&FocusMaxPosNP, nullptr);
			IUUpdateMinMax(&FocusMaxPosNP);

			PresetN[0].value = (int) PresetN[0].value * resolution / last_resolution;
			PresetN[1].value = (int) PresetN[1].value * resolution / last_resolution;
			PresetN[2].value = (int) PresetN[2].value * resolution / last_resolution;
			IDSetNumber(&PresetNP, nullptr);

			getFocuserInfo();

			FocusResolutionSP.s = IPS_OK;
			IDSetSwitch(&FocusResolutionSP, nullptr);
			DEBUGF(INDI::Logger::DBG_SESSION, "Focuser resolution set to 1/%d.", resolution);
			return true;
		}

	        // handle reset absolute position
	        if(!strcmp(name, ResetAbsPosSP.name))
	        {
			IUResetSwitch(&ResetAbsPosSP);

			//set absolute position to zero and save to file
			FocusAbsPosN[0].value = 0;
			IDSetNumber(&FocusAbsPosNP, nullptr);
			savePosition(0);

			DEBUG(INDI::Logger::DBG_SESSION, "Absolute Position reset to 0.");

			ResetAbsPosSP.s = IPS_IDLE;
			IDSetSwitch(&ResetAbsPosSP, nullptr);
			return true;
		}


               // handle changing of the focus steps
                if (!strcmp(name, FocusStepChangeSP.name))
                {
			IUUpdateSwitch(&FocusStepChangeSP, states, names, n);
			int currentSteps = FocusRelPosN[0].value;
			int newSteps = currentSteps;

                        //FOCUS_STEP_DOWN
                        if ( FocusStepChangeS[0].s == ISS_ON )
			{
				if (currentSteps > 200)
				{
					newSteps = 200;
				}
				else if (currentSteps > 100)
				{
					newSteps = 100;
				}
                                else if (currentSteps > 50)
                                {
                                        newSteps = 50;
                                }
                                else if (currentSteps > 25)
                                {
                                        newSteps = 25;
                                }
                                else if (currentSteps > 10)
                                {
                                        newSteps = 10;
                                }
                                else if (currentSteps > 5)
                                {
                                        newSteps = 5;
                                }
                                else
                                {
                                        newSteps = 1;
                                }
			}

                        //FOCUS_STEP_UP
                        if ( FocusStepChangeS[1].s == ISS_ON )
			{
                                if (currentSteps < 5)
                                {
                                        newSteps = 5;
                                }
                                else if (currentSteps < 10)
                                {
                                        newSteps = 10;
                                }
                                else if (currentSteps < 25)
                                {
                                        newSteps = 25;
                                }
                                else if (currentSteps < 50)
                                {
                                        newSteps = 50;
                                }
                                else if (currentSteps < 100)
                                {
                                        newSteps = 100;
                                }
                                else
				{
					newSteps = 200;
                                }
			}

                        FocusRelPosN[0].value = newSteps;

			IDSetSwitch(&FocusStepChangeSP, nullptr);
			FocusStepChangeSP.s = IPS_IDLE;
                        IDSetNumber(&FocusRelPosNP, nullptr);
                        DEBUGF(INDI::Logger::DBG_SESSION, "# Focus Steps changed to %0.0f.", FocusRelPosN[0].value);

                        return true;
                }


	        // handle temperature compensation
	        if(!strcmp(name, TemperatureCompensateSP.name))
	        {
			IUUpdateSwitch(&TemperatureCompensateSP, states, names, n);

			if ( TemperatureCompensateS[0].s == ISS_ON)
			{
				if (!temperatureCompensationID)
					temperatureCompensationID = IEAddTimer(TEMPERATURE_COMPENSATION_TIMEOUT, temperatureCompensationHelper, this);
				TemperatureCompensateSP.s = IPS_OK;
				DEBUG(INDI::Logger::DBG_SESSION, "Temperature compensation ENABLED.");
			}

			if ( TemperatureCompensateS[1].s == ISS_ON)
			{
				IERmTimer(temperatureCompensationID);
				TemperatureCompensateSP.s = IPS_IDLE;
				DEBUG(INDI::Logger::DBG_SESSION, "Temperature compensation DISABLED.");
			}

			IDSetSwitch(&TemperatureCompensateSP, nullptr);
			return true;
		}
	}

	return INDI::Focuser::ISNewSwitch(dev,name,states,names,n);
}

bool AstroberryFocuser::ISNewText (const char *dev, const char *name, char *texts[], char *names[], int n)
{
	// first we check if it's for our device
	if (!strcmp(dev, getDeviceName()))
	{
		// handle active devices
		if (!strcmp(name, ActiveTelescopeTP.name))
		{
				IUUpdateText(&ActiveTelescopeTP,texts,names,n);

				IUFillNumberVector(&ScopeParametersNP, ScopeParametersN, 2, ActiveTelescopeT[0].text, "TELESCOPE_INFO", "Scope Properties", OPTIONS_TAB, IP_RW, 60, IPS_OK);
				IDSnoopDevice(ActiveTelescopeT[0].text, "TELESCOPE_INFO");

				ActiveTelescopeTP.s=IPS_OK;
				IDSetText(&ActiveTelescopeTP, nullptr);
				DEBUGF(INDI::Logger::DBG_SESSION, "Active telescope set to %s.", ActiveTelescopeT[0].text);
				return true;
		}
	}

	return INDI::Focuser::ISNewText(dev,name,texts,names,n);
}

bool AstroberryFocuser::ISSnoopDevice (XMLEle *root)
{
	if (IUSnoopNumber(root, &ScopeParametersNP) == 0)
	{
		getFocuserInfo();
		DEBUGF(INDI::Logger::DBG_DEBUG, "Telescope parameters: %0.0f, %0.0f.", ScopeParametersN[0].value, ScopeParametersN[1].value);
		return true;
	}

	return INDI::Focuser::ISSnoopDevice(root);
}

bool AstroberryFocuser::saveConfigItems(FILE *fp)
{
	IUSaveConfigSwitch(fp, &FocusResolutionSP);
	IUSaveConfigSwitch(fp, &FocusReverseSP);
	IUSaveConfigSwitch(fp, &MotorBoardSP);
	IUSaveConfigSwitch(fp, &TemperatureCompensateSP);
	IUSaveConfigNumber(fp, &FocusMaxPosNP);
	IUSaveConfigNumber(fp, &BCMpinsNP);
	IUSaveConfigNumber(fp, &FocusStepDelayNP);
	IUSaveConfigNumber(fp, &FocusBacklashNP);
	IUSaveConfigNumber(fp, &FocuserTravelNP);
	IUSaveConfigNumber(fp, &PresetNP);
	IUSaveConfigNumber(fp, &TemperatureCoefNP);
	IUSaveConfigText(fp, &ActiveTelescopeTP);
	controller->saveConfigItems(fp);
	return true;
}

bool AstroberryFocuser::AbortFocuser()
{

	DEBUG(INDI::Logger::DBG_SESSION, "Focuser motion aborting.");

	backlashTicksRemaining = 0;
	ticksRemaining = 0;

	return true;
}

void AstroberryFocuser::stepMotor(int direction)
{
	currentStep = currentStep + direction;

	if (currentStep < 0)
	{
		currentStep = 7;
	}

	if (currentStep > 7)
	{
		currentStep = 0;
	}

	if (resolution == 1)
	{	//Full Step
		gpiod_line_set_value(gpio_in1, fullStep[currentStep][0]);
        	gpiod_line_set_value(gpio_in2, fullStep[currentStep][1]);
        	gpiod_line_set_value(gpio_in3, fullStep[currentStep][2]);
        	gpiod_line_set_value(gpio_in4, fullStep[currentStep][3]);
	}
	else if (resolution == 2)
	{	//Half Step
		gpiod_line_set_value(gpio_in1, halfStep[currentStep][0]);
                gpiod_line_set_value(gpio_in2, halfStep[currentStep][1]);
                gpiod_line_set_value(gpio_in3, halfStep[currentStep][2]);
                gpiod_line_set_value(gpio_in4, halfStep[currentStep][3]);
	}
}


void AstroberryFocuser::TimerHit()
{
	if(backlashTicksRemaining <= 0 && ticksRemaining <= 0)
	{
		//All movement completed/aborted
		//save position to file
		savePosition((int) FocusAbsPosN[0].value * MAX_RESOLUTION / resolution); // always save at MAX_RESOLUTION

		// update abspos value and status
		DEBUGF(INDI::Logger::DBG_SESSION, "Focuser at the position %0.0f.", FocusAbsPosN[0].value);

		FocusAbsPosNP.s = IPS_OK;
		IDSetNumber(&FocusAbsPosNP, nullptr);

		// reset last temperature
		lastTemperature = FocusTemperatureN[0].value; // register last temperature

		// set motor standby timer
		IERmTimer(stepperStandbyID);
		stepperStandbyID = IEAddTimer(STEPPER_STANDBY_TIMEOUT, stepperStandbyHelper, this);

		return;
	}

	int motorDirection = lastDirection;

	// handle Reverse Motion
	if (FocusReverseS[INDI_ENABLED].s == ISS_ON) 
	{
		motorDirection = -1 * motorDirection;
	}

	bool isBacklash = false;
	if(backlashTicksRemaining > 0)
	{
		isBacklash = true;
	}


	//Move the actual motor
	stepMotor(motorDirection);

	if(isBacklash == false)
	{//Only Count the position change if it is not due to backlash
		// INWARD - count down
		if ( lastDirection == -1 )
			FocusAbsPosN[0].value -= 1;

		// OUTWARD - count up
		if ( lastDirection == 1 )
			FocusAbsPosN[0].value += 1;

		IDSetNumber(&FocusAbsPosNP, nullptr);

		//decrement counter
		ticksRemaining -= 1;
	}
	else
	{//Don't count the backlash position change, just decrement the counter
		backlashTicksRemaining -= 1;
	}

	SetTimer(FocusStepDelayN[0].value);
}


IPState AstroberryFocuser::MoveRelFocuser(FocusDirection dir, uint32_t ticks)
{
	int targetTicks = FocusAbsPosN[0].value + ((int32_t)ticks * (dir == FOCUS_INWARD ? -1 : 1));
	return MoveAbsFocuser(targetTicks);
}

IPState AstroberryFocuser::MoveAbsFocuser(int targetTicks)
{
	if(backlashTicksRemaining > 0 || ticksRemaining > 0)
        {
                DEBUG(INDI::Logger::DBG_WARNING, "Focuser movement still in progress.");
                return IPS_BUSY;
	}

	if (targetTicks < FocusAbsPosN[0].min || targetTicks > FocusAbsPosN[0].max)
	{
		DEBUG(INDI::Logger::DBG_WARNING, "Requested position is out of range.");
		return IPS_ALERT;
	}

	if (targetTicks == FocusAbsPosN[0].value)
	{
		DEBUG(INDI::Logger::DBG_SESSION, "Already at the requested position.");
		return IPS_OK;
	}

	// set focuser busy
	FocusAbsPosNP.s = IPS_BUSY;
	IDSetNumber(&FocusAbsPosNP, nullptr);

	// check last motion direction for backlash triggering
	int lastdir = lastDirection;

	// set direction
	const char* direction;
	int newDirection;
	if (targetTicks > FocusAbsPosN[0].value)
	{
		// OUTWARD
		direction = "OUTWARD";
		newDirection = 1;
	} else {
		// INWARD
		direction = "INWARD";
		newDirection = -1;
	}

	lastDirection = newDirection;

	// if direction changed do backlash adjustment
	if ( lastdir != 0 && newDirection != lastdir && FocusBacklashN[0].value != 0)
	{
		DEBUGF(INDI::Logger::DBG_SESSION, "Backlash compensation by %0.0f steps.", FocusBacklashN[0].value);
		backlashTicksRemaining = FocusBacklashN[0].value;
	}
	else
	{
		backlashTicksRemaining = 0;
	}

	// process targetTicks
	ticksRemaining = abs(targetTicks - FocusAbsPosN[0].value);

	DEBUGF(INDI::Logger::DBG_SESSION, "Focuser is moving %s to position %d.", direction, targetTicks);

	SetTimer(FocusStepDelayN[0].value);

	return IPS_BUSY;
}

void AstroberryFocuser::SetResolution(int res)
{
	DEBUGF(INDI::Logger::DBG_SESSION, "Resolution set to 1 / %0.0f.", res);
}

bool AstroberryFocuser::ReverseFocuser(bool enabled)
{
	if (enabled)
	{
		DEBUG(INDI::Logger::DBG_SESSION, "Reverse direction ENABLED.");
	} else {
		DEBUG(INDI::Logger::DBG_SESSION, "Reverse direction DISABLED.");
	}
	return true;
}

int AstroberryFocuser::savePosition(int pos)
{
	FILE * pFile;
	char posFileName[MAXRBUF];
	char buf [100];

	if (getenv("INDICONFIG"))
	{
		snprintf(posFileName, MAXRBUF, "%s.position", getenv("INDICONFIG"));
	} else {
		snprintf(posFileName, MAXRBUF, "%s/.indi/%s.position", getenv("HOME"), getDeviceName());
	}


	if (pos == -1)
	{
		pFile = fopen (posFileName,"r");
		if (pFile == NULL)
		{
			DEBUGF(INDI::Logger::DBG_ERROR, "Failed to open file %s.", posFileName);
			return -1;
		}

		fgets (buf , 100, pFile);
		pos = atoi (buf);
		DEBUGF(INDI::Logger::DBG_DEBUG, "Reading position %d from %s.", pos, posFileName);
	} else {
		pFile = fopen (posFileName,"w");
		if (pFile == NULL)
		{
			DEBUGF(INDI::Logger::DBG_ERROR, "Failed to open file %s.", posFileName);
			return -1;
		}

		sprintf(buf, "%d", pos);
		fputs (buf, pFile);
		DEBUGF(INDI::Logger::DBG_DEBUG, "Writing position %s to %s.", buf, posFileName);
	}

	fclose (pFile);

	return pos;
}

bool AstroberryFocuser::readDS18B20()
{
	DIR *dir;
	struct dirent *dirent;
	char dev[16];      // Dev ID
	char devPath[128]; // Path to device
	char buf[256]; // Data from device
	char temperatureData[6]; // Temp C * 1000 reported by device
	char path[] = "/sys/bus/w1/devices";
	ssize_t numRead;
	float tempC/*, tempF*/;

	dir = opendir (path);

	// search for --the first-- DS18B20 device
	if (dir != NULL)
	{
		while ((dirent = readdir (dir)))
		{
			// DS18B20 device is family code beginning with 28-
			if (dirent->d_type == DT_LNK && strstr(dirent->d_name, "28-") != NULL)
			{
				strcpy(dev, dirent->d_name);
				break;
			}
		}
		(void) closedir (dir);
	} else {
		DEBUG(INDI::Logger::DBG_WARNING, "Temperature sensor disabled. 1-Wire interface is not available.");
		return false;
	}

	// Assemble path to --the first-- DS18B20 device
	sprintf(devPath, "%s/%s/w1_slave", path, dev);

	// Opening the device's file triggers new reading
	int fd = open(devPath, O_RDONLY);
	if(fd == -1)
	{
		DEBUG(INDI::Logger::DBG_WARNING, "Temperature sensor not available.");
		return false;
	}

	// set busy
	FocusTemperatureNP.s=IPS_BUSY;
	IDSetNumber(&FocusTemperatureNP, nullptr);

	// read sensor output
	while((numRead = read(fd, buf, 256)) > 0);
	close(fd);

	// parse temperature value from sensor output
	strncpy(temperatureData, strstr(buf, "t=") + 2, 5);
	DEBUGF(INDI::Logger::DBG_DEBUG, "Temperature sensor raw output: %s", buf);
	DEBUGF(INDI::Logger::DBG_DEBUG, "Temperature string: %s", temperatureData);

	tempC = strtof(temperatureData, NULL) / 1000;
	// tempF = (tempC / 1000) * 9 / 5 + 32;

	// check if temperature is reasonable
	if(abs(tempC) > 100)
	{
		DEBUG(INDI::Logger::DBG_DEBUG, "Temperature reading out of range.");
		return false;
	}

	FocusTemperatureN[0].value = tempC;

	// set OK
	FocusTemperatureNP.s=IPS_OK;
	IDSetNumber(&FocusTemperatureNP, nullptr);
	DEBUGF(INDI::Logger::DBG_DEBUG, "Temperature: %.2f°C", tempC);

	return true;
}

bool AstroberryFocuser::SyncFocuser(uint32_t ticks)
{
    FocusAbsPosN[0].value = ticks;
    IDSetNumber(&FocusAbsPosNP, nullptr);
    savePosition(ticks);

    DEBUGF(INDI::Logger::DBG_SESSION, "Absolute Position reset to %0.0f", FocusAbsPosN[0].value);

    return true;
}

void AstroberryFocuser::getFocuserInfo()
{
	// https://www.innovationsforesight.com/education/how-much-focus-error-is-too-much/
	float travel_mm = (float) FocuserTravelN[0].value;
	float aperture = (float) ScopeParametersN[0].value;
	float focal = (float) ScopeParametersN[1].value;
	float f_ratio;

	// handle no snooping data from telescope
	if ( aperture * focal != 0 )
	{
		f_ratio = focal / aperture;
	} else {
		f_ratio =  0;
		DEBUG(INDI::Logger::DBG_DEBUG, "No telescope focal length and/or aperture info available.");
	}

	float cfz = 4.88 * 0.520 * pow(f_ratio, 2); // CFZ = 4.88 · λ · f^2
	float step_size = 1000.0 * travel_mm / FocusMaxPosN[0].value;
	float steps_per_cfz = (int) cfz / step_size;

	if ( steps_per_cfz >= 4  )
	{
		FocuserInfoNP.s = IPS_OK;
	}
	else if ( steps_per_cfz > 2 && steps_per_cfz < 4 )
	{
		FocuserInfoNP.s = IPS_BUSY;
	} else {
		FocuserInfoNP.s = IPS_ALERT;
	}

	FocuserInfoN[0].value = step_size;
	FocuserInfoN[1].value = cfz;
	FocuserInfoN[2].value = steps_per_cfz;
	IDSetNumber(&FocuserInfoNP, nullptr);

	DEBUGF(INDI::Logger::DBG_DEBUG, "Focuser Info: %0.2f %0.2f %0.2f.", FocuserInfoN[0].value, FocuserInfoN[1].value, FocuserInfoN[2].value);
}

void AstroberryFocuser::stepperStandbyHelper(void *context)
{
	static_cast<AstroberryFocuser*>(context)->stepperStandby();
}

void AstroberryFocuser::updateTemperatureHelper(void *context)
{
	static_cast<AstroberryFocuser*>(context)->updateTemperature();
}

void AstroberryFocuser::temperatureCompensationHelper(void *context)
{
	static_cast<AstroberryFocuser*>(context)->temperatureCompensation();
}

void AstroberryFocuser::stepperStandby()
{
	if (!isConnected())
		return;

	gpiod_line_set_value(gpio_in1, 0); // set stepper motor asleep
        gpiod_line_set_value(gpio_in2, 0);
        gpiod_line_set_value(gpio_in3, 0);
        gpiod_line_set_value(gpio_in4, 0);

	DEBUG(INDI::Logger::DBG_SESSION, "Stepper motor going standby.");
}

void AstroberryFocuser::updateTemperature()
{
	if (!isConnected())
		return;

	readDS18B20();
	updateTemperatureID = IEAddTimer(TEMPERATURE_UPDATE_TIMEOUT, updateTemperatureHelper, this);
}

void AstroberryFocuser::temperatureCompensation()
{
	if (!isConnected())
		return;

	if ( TemperatureCompensateS[0].s == ISS_ON && FocusTemperatureN[0].value != lastTemperature )
	{
		float deltaTemperature = FocusTemperatureN[0].value - lastTemperature; // change of temperature from last focuser movement
		float thermalExpansionRatio = TemperatureCoefN[0].value * ScopeParametersN[1].value / 1000; // termal expansion in micrometers per 1 celcius degree
		float thermalExpansion = thermalExpansionRatio * deltaTemperature; // actual thermal expansion

		DEBUGF(INDI::Logger::DBG_DEBUG, "Thermal expansion of %0.1f μm due to temperature change of %0.2f°C", thermalExpansion, deltaTemperature);

		if ( abs(thermalExpansion) > FocuserInfoN[1].value / 2)
		{
			int thermalAdjustment = round((thermalExpansion / FocuserInfoN[0].value) / 2); // adjust focuser by half number of steps to keep it in the center of cfz
			MoveAbsFocuser(FocusAbsPosN[0].value + thermalAdjustment); // adjust focuser position
			lastTemperature = FocusTemperatureN[0].value; // register last temperature
			DEBUGF(INDI::Logger::DBG_SESSION, "Focuser adjusted by %d steps due to temperature change by %0.2f°C", thermalAdjustment, deltaTemperature);
		}
	}

	temperatureCompensationID = IEAddTimer(TEMPERATURE_COMPENSATION_TIMEOUT, temperatureCompensationHelper, this);
}
