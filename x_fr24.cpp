/* Copyright (C) 2015  Matteo Hausner
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "api.h"
#include "XPLMDataAccess.h"
#include "XPLMDisplay.h"
#include "XPLMGraphics.h"
#include "XPLMProcessing.h"
#include "XPLMScenery.h"
#include "XPLMUtilities.h" //TODO Remove

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h> //TODO Remove

// define name
#define NAME "X-fr24"
#define NAME_LOWERCASE "x_fr24"

// define version
#define VERSION "0.1"

// define maximum number of planes
#define MAX_PLANES 64

// define obj path
#define OBJ_PATH "Resources/default scenery/sim objects/apt_aircraft/heavy_metal/MD-80_Scandinavian/MD80_SAS.obj"

// define factor knots to meters per second
#define FACTOR_FEET_TO_METERS 0.3048
#define FACTOR_KNOTS_TO_METERS_PER_SECOND 0.514444
#define MAX_SNAP_TO_GROUND_ALTITUDE 5.0 // feet AGL

// global dataref variables
static XPLMDataRef latitudeDataRef = NULL, longitudeDataRef = NULL, elevationDataRef = NULL, earthRadiusMDataRef = NULL;

// global internal variables
static XPLMObjectRef object = NULL;
static XPLMProbeRef probe = NULL;

// converts from degrees to radians
inline static double DegreesToRadians(double degrees)
{
    return degrees * (M_PI / 180.0);
}

// converts from degrees to radians
inline static double RadiansToDegrees(double radians)
{
    return radians * (180.0 / M_PI);
}

// calculates the destination point given distance and bearing from a starting point
static void GetDestinationPoint(double *destinationLatitude, double *destinationLongitude, double startLatitude, double startLongitude, double distance, double bearing)
{
    double d = (double) (distance / XPLMGetDataf(earthRadiusMDataRef));

    *destinationLatitude = RadiansToDegrees(asin(sin(DegreesToRadians(startLatitude)) * cos(d) + cos(DegreesToRadians(startLatitude)) * sin(d) * cos(DegreesToRadians(bearing))));
    *destinationLongitude = RadiansToDegrees(DegreesToRadians(startLongitude) + atan2(sin(DegreesToRadians(bearing)) * sin(d) * cos(DegreesToRadians(startLatitude)), cos(d) - sin(DegreesToRadians(startLatitude)) * sin(DegreesToRadians(*destinationLatitude))));
}

// normalizes a given two dimensional vector
static void NormalizeVector(double *x, double *y)
{
    double length = sqrt(pow(*x, 2) + pow(*y, 2));

    if(length != 0)
    {
        *x = *x / length;
        *y = *y / length;
    }
}

// flightloop-callback that interpolates the planes' positions between updates
static float FlightLoopCallback(float inElapsedSinceLastCall, float inElapsedTimeSinceLastFlightLoop, int inCounter, void *inRefcon)
{
    if (probe == NULL)
        probe = XPLMCreateProbe(xplm_ProbeY);

    SetPosition(XPLMGetDataf(latitudeDataRef), XPLMGetDataf(longitudeDataRef));

    char o[1024];
    sprintf(o, "Count = %d\n", (int) planes.size());
    XPLMDebugString(o);

    pthread_mutex_lock(&planesMutex);
    for (std::map<std::string, Plane*>::iterator p = planes.begin(); p != planes.end(); ++p)
    {
        Plane *plane = p->second;
        double distance = ((double) plane->speed * FACTOR_KNOTS_TO_METERS_PER_SECOND) * (double) inElapsedSinceLastCall; // in meters

        double latitude = 0.0;
        if (plane->interpolatedLatitude != 0.0)
            latitude = plane->interpolatedLatitude;
        else
            latitude = plane->latitude;

        double longitude = 0.0;
        if (plane->interpolatedLongitude != 0.0)
            longitude = plane->interpolatedLongitude;
        else
            longitude = plane->longitude;

        double altitude = 0.0;
        if (plane->interpolatedAltitude != -1000.0)
            altitude = plane->interpolatedAltitude;
        else
            altitude = plane->altitude;

        double newLatitude = 0.0f, newLongitude = 0.0f;
        GetDestinationPoint(&newLatitude, &newLongitude, latitude, longitude, distance, plane->heading);

        double newAltitude = altitude + ((double) plane->verticalSpeed / 60.0) * (double) inElapsedSinceLastCall;

        XPLMProbeInfo_t info;
        info.structSize = sizeof(info);
        double x = 0.0, y = 0.0, z = 0.0;
        XPLMWorldToLocal(newLatitude, newLongitude, 0.0, &x, &y, &z);

        if (XPLMProbeTerrainXYZ(probe, x, y, z, &info) == xplm_ProbeHitTerrain)
        {

            double unusedLatitude = 0.0, unusedLongitude = 0.0, terrainElevation = 0.0; // in meters
            XPLMLocalToWorld(info.locationX, info.locationY, info.locationZ, &unusedLatitude, &unusedLongitude, &terrainElevation);
            terrainElevation /= FACTOR_FEET_TO_METERS; // convert to feet

            if (newAltitude < terrainElevation || newAltitude - terrainElevation <= MAX_SNAP_TO_GROUND_ALTITUDE)
                newAltitude = terrainElevation;
        }

        double vX = distance, vY = newAltitude - altitude;
        NormalizeVector(&vX, &vY);
        float newPitch = RadiansToDegrees(asin(vY));

        char out[1024];
        sprintf(out, "%s:\n Distance = %f\n Old Lat = %f\n New Lat = %f\n Old Lon = %f\n New Lon = %f\n Old Pitch = %f\n New Pitch = %f\n Old Alt = %f\n New Alt = %f\n VS = % d\n", p->first.c_str(), distance, latitude, longitude, newLatitude, newLongitude, plane->pitch, newPitch, altitude, newAltitude, plane->verticalSpeed);
        XPLMDebugString(out);

        plane->interpolatedLatitude = newLatitude;
        plane->interpolatedLongitude = newLongitude;
        plane->interpolatedAltitude = newAltitude;
        plane->pitch = newPitch;
    }
    pthread_mutex_unlock(&planesMutex);

    return 0.01f;
}

// draw-callback that performs the actual drawing of the planes
static int DrawCallback(XPLMDrawingPhase inPhase, int inIsBefore, void *inRefcon)
{
    pthread_mutex_lock(&planesMutex);
    for (std::map<std::string, Plane*>::iterator p = planes.begin(); p != planes.end(); ++p)
    {
        /*char o[1024];
        sprintf(o, "Rendering: %s\n", p->first.c_str());
        XPLMDebugString(o);*/
        Plane *plane = p->second;
        double x = 0.0, y = 0.0, z = 0.0;
        XPLMWorldToLocal(plane->interpolatedLatitude, plane->interpolatedLongitude, plane->interpolatedAltitude * FACTOR_FEET_TO_METERS, &x, &y, &z);

        XPLMDrawInfo_t locations[1] = {0};
        locations[0].structSize = sizeof(XPLMDrawInfo_t);
        locations[0].x = (float) x;
        locations[0].y = (float) y;
        locations[0].z = (float) z;
        locations[0].pitch = plane->roll; //TODO temp correction for obj orientation!
        locations[0].heading = plane->heading + 90.0f; //TODO temp correction for obj orientation!
        locations[0].roll = plane->pitch; //TODO temp correction for obj orientation!

        if (object != NULL)
            XPLMDrawObjects(object, 1, locations, 0, 1);
    }
    pthread_mutex_unlock(&planesMutex);


    return 1;
}

PLUGIN_API int XPluginStart(char *outName, char *outSig, char *outDesc)
{
    // set plugin info
    strcpy(outName, NAME);
    strcpy(outSig, "de.bwravencl." NAME_LOWERCASE);
    strcpy(outDesc, NAME " displays live air traffic from Flightradar24 in X-Plane!");

    // obtain datarefs
    latitudeDataRef = XPLMFindDataRef("sim/flightmodel/position/latitude");
    longitudeDataRef = XPLMFindDataRef("sim/flightmodel/position/longitude");
    elevationDataRef = XPLMFindDataRef("sim/flightmodel/position/elevation");
    earthRadiusMDataRef = XPLMFindDataRef("sim/physics/earth_radius_m");

    // load object
    object = XPLMLoadObject(OBJ_PATH);
    if (object == NULL)
        XPLMDebugString("Error: object not found!\n");

    Init();

    // register flight loop callbacks
    XPLMRegisterFlightLoopCallback(FlightLoopCallback, -1, NULL);

    // register draw callback
    XPLMRegisterDrawCallback(DrawCallback, xplm_Phase_Objects, 0, NULL);

    return 1;
}

PLUGIN_API void	XPluginStop(void)
{
    if (probe != NULL)
        XPLMDestroyProbe(probe);

    if (object != NULL)
        XPLMUnloadObject(object);

    Cleanup();
}

PLUGIN_API void XPluginDisable(void)
{
}

PLUGIN_API int XPluginEnable(void)
{
    return 1;
}

PLUGIN_API void XPluginReceiveMessage(XPLMPluginID inFromWho, long inMessage, void *inParam)
{
}
