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

#include "XPLMDataAccess.h"
#include "XPLMDisplay.h"
#include "XPLMGraphics.h"
#include "XPLMProcessing.h"
#include "XPLMScenery.h"
#include "XPLMUtilities.h" //TODO Remove

//#include "jsmn/jsmn.h"
//#include "jsmn/jsmn.c"
#include "parson.h"

#include <curl/curl.h>
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

// global dataref variables
static XPLMDataRef latitudeDataRef = NULL, longitudeDataRef = NULL, elevationDataRef = NULL, earthRadiusMDataRef = NULL;

// plane struct
struct Plane
{
    char id[8];
    double latitude; // degrees
    double longitude; // degrees
    double altitude; // feet MSL
    float pitch; // degrees
    float roll; // degrees
    float heading; // degrees
    int speed; // knots
    int verticalSpeed; // feet per minute
};

// global internal variables
static struct Plane* planes[MAX_PLANES] = {NULL};
static XPLMObjectRef object = NULL;

struct url_data
{
    size_t size;
    char* data;
};

static size_t WriteData(void *ptr, size_t size, size_t nmemb, struct url_data *data)
{
    size_t index = data->size;
    size_t n = (size * nmemb);
    char* tmp;
    
    data->size += (size * nmemb);
    
    tmp = (char*) realloc(data->data, data->size + 1); /* +1 for '\0' */
    
    if(tmp)
        data->data = tmp;
    else {
        if(data->data)
            free(data->data);
        
        return 0;
    }
    
    memcpy((data->data + index), ptr, n);
    data->data[data->size] = '\0';
    
    return size * nmemb;
}

static char *GetUrl(const char* url)
{
    CURL *curl = NULL;
    
    url_data data;
    data.size = 0;
    data.data = (char*) malloc(4096);
    if(NULL == data.data)
        return NULL;
    
    data.data[0] = '\0';
    
    CURLcode res;
    
    curl = curl_easy_init();
    if (curl)
    {
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteData);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);
        res = curl_easy_perform(curl);
        
        curl_easy_cleanup(curl);
        
    }

    return data.data;
}

struct Server
{
    char url[128];
    int load;
};

static void FetchData();

/*static void FetchData(void)
{
    char *balance = GetUrl("http://www.flightradar24.com/balance.json");
    XPLMDebugString(balance);
    
    jsmn_parser parser;
    jsmn_init(&parser);

    jsmntok_t tokens[256];
    const char *js;
    jsmnerr_t r = jsmn_parse(&parser, balance, strlen(balance), tokens, sizeof(tokens) / sizeof(tokens[0]));

    if (r < 0)
    {
//        printf("Failed to parse JSON: %d\n", r);
	return;
    }

    if (r < 1 || tokens[0].type != JSMN_OBJECT)
    {
//        printf("Object expected\n");
        return;
    }

    Server* servers[16] = {NULL};
    int serverCount = 0;

    for (int i = 1; i < r; i++)
    {
        if (tokens[i].type == JSMN_PRIMITIVE && tokens[i - 1].type == JSMN_STRING)
        {
            Server *s = (Server*) malloc(sizeof(*s));
            memcpy(s->url, &balance[tokens[i - 1].start], tokens[i - 1].size);
            s->url[tokens[i - 1].size] = '\0';

            s->load = (int) strtol(&balance[tokens[i].start], NULL, 10);
            servers[serverCount] = s;
            serverCount++;

            char out[1024];
            sprintf(out, "Found Server %d:\n URL = %s; LOAD = %d", serverCount, s->url, s->load);
        }
    }

    if (serverCount < 1)
    {
        XPLMDebugString("Error: No servers found!");
        return;
    }

    Server *server = servers[0];
    for (int i = 1; i < serverCount; i++)
    {
        if (servers[i]->load < server->load)
            server = servers[i];
    }

    char out[1024];
    sprintf(out, "Best server: %s\n", server->url);
    XPLMDebugString(out);

    Plane *p = (Plane*) malloc(sizeof(*p)); 
    strcpy(p->id, "685db8c");
    p->latitude = 47.862542;
    p->longitude = -119.947726;
    p->altitude = 1280.0;
    p->pitch = 0.0f;
    p->roll = 0.0f;
    p->heading = 43.62f;
    p->speed = 180;
    p->verticalSpeed = 2000;

    if (planes[0] != NULL)
        free(planes[0]);
    planes[0] = p;
}*/

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
    for (int i = 0; i < MAX_PLANES; i++)
    {
        Plane *p = planes[i];

        if (p != NULL)
        {
            double distance = ((double) p->speed * FACTOR_KNOTS_TO_METERS_PER_SECOND) * (double) inElapsedSinceLastCall; // in meters

            double newLatitude = 0.0f, newLongitude = 0.0f;
            GetDestinationPoint(&newLatitude, &newLongitude, p->latitude, p->longitude, distance, p->heading);


            double newAltitude = p->altitude + ((double) p->verticalSpeed / 60.0) * (double) inElapsedSinceLastCall;

            double x = distance, y = newAltitude - p->altitude;
            NormalizeVector(&x, &y);
            float newPitch = RadiansToDegrees(asin(y));

            char out[1024];
            sprintf(out, "%s:\n Distance = %f\n Old Lat = %f\n New Lat = %f\n Old Lon = %f\n New Lon = %f\n Old Pitch = %f\n New Pitch = %f\n", p->id, distance, p->latitude, p->longitude, newLatitude, newLongitude, p->pitch, newPitch);
            //XPLMDebugString(out);

            p->latitude = newLatitude;
            p->longitude = newLongitude;
            p->altitude = newAltitude;
            p->pitch = newPitch;
        }
    }

    return 0.01f;
}

// draw-callback that performs the actual drawing of the planes
static int DrawCallback(XPLMDrawingPhase inPhase, int inIsBefore, void *inRefcon)
{
    for (int i = 0; i < MAX_PLANES; i++)
    {
        Plane *p = planes[i];

        if (p != NULL)
        {
            double x = 0.0, y = 0.0, z = 0.0;
            XPLMWorldToLocal(p->latitude, p->longitude, p->altitude * FACTOR_FEET_TO_METERS, &x, &y, &z);

            XPLMDrawInfo_t locations[1] = {0};
            locations[0].structSize = sizeof(XPLMDrawInfo_t);
            locations[0].x = (float) x;
            locations[0].y = (float) y;
            locations[0].z = (float) z;
            locations[0].pitch = p->roll; //TODO temp correction for obj orientation!
            locations[0].heading = p->heading + 90.0f; //TODO temp correction for obj orientation!
            locations[0].roll = p->pitch; //TODO temp correction for obj orientation!
            
            if (object != NULL)
                XPLMDrawObjects(object, 1, locations, 0, 1);
        }
    }

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

    // register flight loop callbacks
    XPLMRegisterFlightLoopCallback(FlightLoopCallback, -1, NULL);

    // register draw callback
    XPLMRegisterDrawCallback(DrawCallback, xplm_Phase_Objects, 0, NULL);

    FetchData();

    return 1;
}

PLUGIN_API void	XPluginStop(void)
{
    if (object != NULL)
        XPLMUnloadObject(object);

    for (int i = 0; i < MAX_PLANES; i++)
    {
        if (planes[i] != NULL)
            free(planes[i]);
    }
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
