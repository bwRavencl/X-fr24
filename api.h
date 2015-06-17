#ifndef API_H
#define API_H

#include <map>
#include <string>
#include <time.h>

// define plane struct
struct Plane
{
    char registration[10]; // registration number
    char icaoId[9]; // ICAO flight ID
    char icaoType[5]; // ICAO aircraft type designator
    char squawk[5]; // squawk code
    double latitude; // degrees
    double longitude; // degrees
    double altitude; // feet MSL
    float pitch; // degrees
    float roll; // degrees
    float heading; // degrees
    int speed; // knots
    int verticalSpeed; // feet per minute
    time_t lastSeen; // seconds
};

extern std::map<std::string, Plane*> planes;
extern pthread_mutex_t planesMutex;

// provides safe writing access to the users position
void SetPosition(double latitude, double longitude);

// initilializes the update thread and the mutexes
void Init(void);

// destroys the mutexes
void Cleanup(void);

#endif
