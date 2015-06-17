#include "parson/parson.h"

#include <curl/curl.h>
#include <map>
#include <math.h>
#include <stdlib.h>
#include <pthread.h>
#include <string>
#include <string.h>
#include <unistd.h>

// define maximum viewing distance in nautical miles
#define MAX_DISTANCE 20.0

// define intervall in seconds after which a plane is removed if there is no more data about it
#define PLANE_TIMEOUT 30

// define URLs
#define URL_BALANCE "http://www.flightradar24.com/balance.json"
#define URL_ZONES "http://www.flightradar24.com/js/zones.js.php"
#define URL_ZONE_INFIX "/zones/fcgi/"
#define URL_ZONE_SUFFIX "_all.json"

// define indices of relevant aircraft properties for parsing
#define ARRAY_INDEX_LATITUDE 1
#define ARRAY_INDEX_LONGITUDE 2
#define ARRAY_INDEX_HEADING 3
#define ARRAY_INDEX_ALTITUDE 4
#define ARRAY_INDEX_SPEED 5
#define ARRAY_INDEX_SQUAWK 6
#define ARRAY_INDEX_ICAO_TYPE 8
#define ARRAY_INDEX_REGISTRATION 9
#define ARRAY_INDEX_VERTICAL_SPEED 16
#define ARRAY_INDEX_ICAO_ID 17

// define radius of the earth in nautical miles
#define RADIUS_EARTH 3440.07

// plane struct
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

//static double latitude = 0.0, longitude = 0.0;
//static double myLatitude = 51.5286416 , myLongitude = -0.1015987; // London
//static double myLatitude = 50.4433421, myLongitude = -4.9443987; // Cornwall
static double myLatitude = 48.3537449, myLongitude = 11.7860028; // Munich
//static double myLatitude = 50.9987326, myLongitude = 13.9867738; // Dresden
//static double myLatitude = 37.7577, myLongitude = -122.4376; // San Francisco
//static double myLatitude = 40.7033127, myLongitude = -73.979681; // New York
//static double myLatitude = 34.0204989, myLongitude = -118.4117325; // Los Angeles
//static double myLatitude = 49.8152995, myLongitude = 6.13332; // Luxemburg
//static double myLatitude = -33.7969235, myLongitude = 150.9224326; //Sydney
//static double myLatitude = 47.4812134, myLongitude = 19.1303031; // Budapest
std::map<std::string, Plane*> planes;

pthread_mutex_t mutex;

// define UrlData struct used by libcurl
struct UrlData
{
    size_t size;
    char* data;
};

// WriteData function used by libcurl
static size_t WriteData(void *ptr, size_t size, size_t nmemb, UrlData *data)
{
    size_t index = data->size;
    size_t n = (size * nmemb);

    data->size += (size * nmemb);

    char *tmp = (char*) realloc(data->data, data->size + 1);

    if(tmp != NULL)
        data->data = tmp;
    else
    {
        if(data->data != NULL)
        {
            free(data->data);
            data->data = NULL;
        }

        return 0;
    }

    memcpy((data->data + index), ptr, n);
    data->data[data->size] = '\0';

    return size * nmemb;
}

// retrieves a url with libcurl and returns the content
static char* GetUrl(const char* url)
{
    CURL *curl = NULL;

    UrlData data;
    data.size = 0;
    data.data = (char*) malloc(4096);

    if(data.data != NULL)
    {
        data.data[0] = '\0';

        curl = curl_easy_init();
        if (curl != NULL)
        {
            curl_easy_setopt(curl, CURLOPT_URL, url);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteData);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);
            curl_easy_perform(curl);
            curl_easy_cleanup(curl);
        }
    }

    return data.data;
}

// define json_value_value type used by Parson
typedef union json_value_value
{
    char *string;
    double number;
    JSON_Object *object;
    JSON_Array *array;
    int boolean;
    int null;
} JSON_Value_Value;

// define json_value_t struct used by Parson
struct json_value_t
{
    JSON_Value_Type type;
    JSON_Value_Value value;
};

// retrieves the balancer url with the lowest load, if there are several balancers with the same load one of these is randomly selected
static char* GetBalancerUrl(void)
{
    char *bestUrl = NULL;

    char *balance = GetUrl(URL_BALANCE);
    if (balance != NULL)
    {
        JSON_Value *rootJson = json_parse_string(balance);
        free(balance);
        balance = NULL;

        if (rootJson != NULL && rootJson->type == JSONObject)
        {
            JSON_Object *serversJson = json_value_get_object(rootJson);

            if (serversJson != NULL)
            {
                size_t serverCount = json_object_get_count(serversJson);
                int bestLoad = 0;

                for (int i = 0; i < serverCount; i++)
                {
                    const char *url = json_object_get_name(serversJson, i);

                    JSON_Value *value = json_object_get_value(serversJson, url);

                    if (value != NULL && value->type == JSONNumber)
                    {
                        int load = (int) json_object_get_number(serversJson, url);

                        if (load != 0 && (bestUrl == NULL || load < bestLoad || (load == bestLoad && rand() % 2)))
                        {
                            if (bestUrl != NULL)
                            {
                                free(bestUrl);
                                bestUrl = NULL;
                            }

                            bestUrl = (char*) malloc(strlen(url) + 1);
                            if (bestUrl != NULL)
                                strcpy(bestUrl, url);

                            bestLoad = load;
                        }
                    }
                }
            }

            json_value_free(rootJson);
            rootJson = NULL;
        }
    }

    return bestUrl;
}

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

// calculates the midpoint between two given coordinates
void GetMidpoint(double *latitudeMidpoint, double *longitudeMidpoint, double latiudeA, double longitudeA, double latiudeB, double longitudeB)
{
    double dLongitude = DegreesToRadians(longitudeB - longitudeA);
    double bX = cos(DegreesToRadians(latiudeB)) * cos(dLongitude);
    double bY = cos(DegreesToRadians(latiudeB)) * sin(dLongitude);

    *latitudeMidpoint = RadiansToDegrees(atan2(sin(DegreesToRadians(latiudeA)) + sin(DegreesToRadians(latiudeB)), sqrt((cos(DegreesToRadians(latiudeA)) + bX) * (cos(DegreesToRadians(latiudeA)) + bX) + bY * bY)));

    *longitudeMidpoint = longitudeA + RadiansToDegrees(atan2(bY, cos(DegreesToRadians(latiudeA)) + bX));
}

// calculates the distance between two coordinates in nautical miles
double GetDistance(double latiudeA, double longitudeA, double latiudeB, double longitudeB)
{
    double dLatitude = latiudeB - latiudeA;
    double dLongitude = longitudeB - longitudeA;

    double a = pow(sin(DegreesToRadians(dLatitude / 2.0)), 2) + cos(DegreesToRadians(latiudeA)) * cos(DegreesToRadians(latiudeB)) * pow(sin(DegreesToRadians(dLongitude / 2.0)), 2);
    double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));

    return c * RADIUS_EARTH;
}

// parses a JSON object containing zones and calculates the zone that fits the given latitude and longitude best, bestDistance is used internally and contains the distance from the midpoint of the selected zone
static void ParseZones(char **bestZone, double *bestDistance, JSON_Object *zonesJson, double latitude, double longitude)
{
    *bestDistance = -1.0;

    if (zonesJson != NULL)
    {
        size_t zoneCount = json_object_get_count(zonesJson);

        for (int i = 0; i < zoneCount; i++)
        {
            const char *zoneName = json_object_get_name(zonesJson, i);
            JSON_Value *valueJson = json_object_get_value(zonesJson, zoneName);

            if (valueJson != NULL && valueJson->type == JSONObject)
            {
                JSON_Object *zoneJson = json_value_get_object(valueJson);
                double topLeftX = 0.0, topLeftY = 0.0, bottomRightX = 0.0, bottomRightY = 0.0;
                JSON_Object *subzonesJson = NULL;

                size_t propertyCount = json_object_get_count(zoneJson);
                for (int j = 0; j < propertyCount; j++)
                {
                    const char *propertyName = json_object_get_name(zoneJson, j);
                    JSON_Value *propertyJson = json_object_get_value(zoneJson, propertyName);

                    if (propertyJson != NULL)
                    {
                        if (propertyJson->type == JSONNumber)
                        {
                            double number = json_value_get_number(propertyJson);

                            if (strcmp(propertyName, "tl_x") == 0)
                                topLeftX = number;
                            else if (strcmp(propertyName, "tl_y") == 0)
                                topLeftY = number;
                            else if (strcmp(propertyName, "br_x") == 0)
                                bottomRightX = number;
                            else if (strcmp(propertyName, "br_y") == 0)
                                bottomRightY = number;
                        }
                        else if (propertyJson->type == JSONObject && (strcmp(propertyName, "subzones") == 0))
                            subzonesJson = json_value_get_object(propertyJson);
                    }
                }

                if (topLeftX != 0.0 && topLeftY != 0.0 && bottomRightX != 0.0 && bottomRightY != 0.0 && longitude > topLeftX && latitude < topLeftY && longitude < bottomRightX && latitude > bottomRightY)
                {
                    double latitudeMidpoint = 0.0, longitudeMidpoint = 0.0;
                    GetMidpoint(&latitudeMidpoint, &longitudeMidpoint, topLeftY, topLeftX, bottomRightY, bottomRightX);
                    double distance = GetDistance(latitudeMidpoint, longitudeMidpoint, latitude, longitude);
//                    printf("Distance from %s = %f\n", zoneName, distance);
                    if (*bestDistance == -1.0 || distance < *bestDistance)
                    {
                        if (*bestZone != NULL)
                        {
                            free(*bestZone);
                            *bestZone = NULL;
                        }

                        if (subzonesJson != NULL)
                        {
                            char *bestSubzone = NULL;
                            double subzoneDistance = -1.0;
                            ParseZones(&bestSubzone, &subzoneDistance, subzonesJson, latitude, longitude);

                            if (bestSubzone != NULL && subzoneDistance != -1.0)
                            {
                                *bestDistance = subzoneDistance;
                                *bestZone = bestSubzone;
                            }
                        }

                        if (*bestZone == NULL)
                        {
                            *bestDistance = distance;
                            *bestZone = (char*) malloc(strlen(zoneName) + 1);
                            if (*bestZone != NULL)
                                strcpy(*bestZone, zoneName);
                        }
                    }
                }
            }
        }
    }
}

// returns the name of the zone that fits the given latitude and longitude best
static char* GetZoneName(double latitude, double longitude)
{
    char *zoneName = NULL;
    char *zones = GetUrl(URL_ZONES);

    if (zones != NULL)
    {
        JSON_Value *rootJson = json_parse_string(zones);
        free(zones);
        zones = NULL;

        if (rootJson != NULL)
        {
            if (rootJson->type == JSONObject)
            {
                double bestDistance = -1.0;
                ParseZones(&zoneName, &bestDistance, json_value_get_object(rootJson), latitude, longitude);
            }

            json_value_free(rootJson);
            rootJson = NULL;
        }
    }

    return zoneName;
}

// updates the planes map, planes not seen for a defined intervall are removed from the map and only planes within a defined distance from the given latitude and longited
void UpdatePlanes(char *balancerUrl, char *zoneName, double latitude, double longitude)
{
    time_t currentTime = time(NULL);

    if (currentTime != ((time_t) -1))
    {

        pthread_mutex_lock(&mutex);
        for (std::map<std::string, Plane*>::iterator p = planes.begin(); p != planes.end(); ++p)
        {
            Plane *plane = p->second;
            if (currentTime - plane->lastSeen > PLANE_TIMEOUT)
            {
                //printf("Removing: %s - CurrentTime = %d - LastSeen = %d\n", p->first.c_str(), (int) currentTime, (int) plane->lastSeen);
                free(plane);
                plane = NULL;
                planes.erase(p->first);
            }
        }
        pthread_mutex_unlock(&mutex);

        char url[strlen(balancerUrl) + strlen(URL_ZONE_INFIX) + strlen(zoneName) + strlen(URL_ZONE_SUFFIX)];
        sprintf(url, "%s%s%s%s", balancerUrl, URL_ZONE_INFIX, zoneName, URL_ZONE_SUFFIX);

        char *zone = GetUrl(url);
        if (zone != NULL)
        {
            JSON_Value *rootJson = json_parse_string(zone);
            free(zone);
            zone = NULL;

            if (rootJson != NULL)
            {
                if (rootJson->type == JSONObject)
                {
                    JSON_Object *aircraftJson = json_value_get_object(rootJson);
                    if (aircraftJson != NULL)
                    {
                        size_t aircraftCount = json_object_get_count(aircraftJson);
                        for (int i = 0; i < aircraftCount; i++)
                        {
                            const char *id = json_object_get_name(aircraftJson, i);
                            if (id != NULL)
                            {
                                JSON_Value *value = json_object_get_value(aircraftJson, id);

                                if (value != NULL && value->type == JSONArray)
                                {
                                    JSON_Array *propertiesJson = json_value_get_array(value);

                                    if (propertiesJson != NULL)
                                    {
                                        const char *registration = NULL, *icaoId = NULL, *icaoType = NULL, *squawk = NULL;
                                        double latitudePlane = 0.0, longitudePlane = 0.0, altitude = 0.0;
                                        float heading = 0.0f;
                                        int speed = 0, verticalSpeed = 0;

                                        int propertyCount = json_array_get_count(propertiesJson);
                                        for (int k = 0; k < propertyCount; k++)
                                        {
                                            JSON_Value *valueJson = json_array_get_value(propertiesJson, k);

                                            if (valueJson != NULL)
                                            {
                                                switch (k)
                                                {
                                                case ARRAY_INDEX_LATITUDE:
                                                    latitudePlane = json_value_get_number(valueJson);
                                                    break;
                                                case ARRAY_INDEX_LONGITUDE:
                                                    longitudePlane = json_value_get_number(valueJson);
                                                    break;
                                                case ARRAY_INDEX_ALTITUDE:
                                                    altitude = json_value_get_number(valueJson);
                                                    break;
                                                case ARRAY_INDEX_HEADING:
                                                    heading = (float) json_value_get_number(valueJson);
                                                    break;
                                                case ARRAY_INDEX_SPEED:
                                                    speed = (int) json_value_get_number(valueJson);
                                                    break;
                                                case ARRAY_INDEX_SQUAWK:
                                                    squawk = json_value_get_string(valueJson);
                                                    break;
                                                case ARRAY_INDEX_ICAO_TYPE:
                                                    icaoType = json_value_get_string(valueJson);
                                                    break;
                                                case ARRAY_INDEX_REGISTRATION:
                                                    registration = json_value_get_string(valueJson);
                                                    break;
                                                case ARRAY_INDEX_VERTICAL_SPEED:
                                                    verticalSpeed = (int) json_value_get_number(valueJson);
                                                    break;
                                                case ARRAY_INDEX_ICAO_ID:
                                                    icaoId = json_value_get_string(valueJson);
                                                    break;
                                                }
                                            }
                                        }

                                        if (latitudePlane != 0.0 && longitudePlane != 0.0 && GetDistance(latitude, longitude, latitudePlane, longitudePlane) <= MAX_DISTANCE)
                                        {
                                            Plane *plane = NULL;

                                            pthread_mutex_lock(&mutex);
                                            std::map<std::string, Plane*>::iterator p = planes.find(id);
                                            if (p != planes.end())
                                                plane = p->second;
                                            else
                                            {
                                                plane = (Plane*) malloc(sizeof(*plane));
                                                if (plane != NULL)
                                                    planes[std::string(id)] = plane;
                                            }

                                            if (plane != NULL)
                                            {
                                                if (registration == NULL || strlen(registration) == 0)
                                                    registration = "Unknown";
                                                if (icaoId == NULL || strlen(icaoId) == 0)
                                                    icaoId = "Unknown";
                                                if (icaoType == NULL || strlen(icaoType) == 0)
                                                    icaoType = "UKN";
                                                if (squawk == NULL || strlen(squawk) == 0)
                                                    squawk = "0000";
                                                strncpy(plane->registration, registration, sizeof(plane->registration) / sizeof(char));
                                                strncpy(plane->icaoId, icaoId, sizeof(plane->icaoId) / sizeof(char));
                                                strncpy(plane->icaoType, icaoType, sizeof(plane->icaoType) / sizeof(char));
                                                strncpy(plane->squawk, squawk, sizeof(plane->squawk) / sizeof(char));
                                                plane->latitude = latitudePlane;
                                                plane->longitude = longitudePlane;
                                                plane->altitude = altitude;
                                                plane->pitch = 0.0f;
                                                plane->roll = 0.0f;
                                                plane->heading = heading;
                                                plane->speed = speed;
                                                plane->verticalSpeed = verticalSpeed;
                                                plane->lastSeen = currentTime;
                                            }
                                            pthread_mutex_unlock(&mutex);
                                        }
                                    }
                                }
                            }
                        }
                    }

                    json_value_free(rootJson);
                    rootJson = NULL;
                }
            }
        }
    }
}

void *UpdateThreadFunction(void *ptr)
{
    srand(time(NULL));

    while (true)
    {
        double latitude = myLatitude, longitude = myLongitude;

        char *balancerUrl = GetBalancerUrl();
//        printf("URL -> %s\n", balancerUrl);
        if (balancerUrl != NULL)
        {
            char *zoneName = GetZoneName(latitude, longitude);
            if (zoneName != NULL)
            {
//                printf("ZONE -> %s\n", zoneName);
                UpdatePlanes(balancerUrl, zoneName, latitude, longitude);
                free(zoneName);
                zoneName = NULL;
            }

            free(balancerUrl);
            balancerUrl = NULL;
        }

        sleep(3);
    }
}

int main(void)
{
    pthread_t thread = 0;
    pthread_mutex_init(&mutex, 0);
    pthread_create(&thread, NULL, UpdateThreadFunction, NULL);

    while(true)
    {
        printf("\033[2J\033[1;1H");
        printf("PlaneCount = %d\n\n", (int) planes.size());

        pthread_mutex_lock(&mutex);
        for (std::map<std::string, Plane*>::iterator p = planes.begin(); p != planes.end(); ++p)
        {
            Plane *plane = p->second;
            printf("Plane %s:\n Registration = %s\n ICAO ID = %s\n ICAO Type = %s\n Squawk = %s\n Latitude = %f\n Longitude = %f\n Altitude = %f\n Heading = %f\n Speed = %d\n Vertical Speed = %d\n\n", p->first.c_str(), plane->registration, plane->icaoId, plane->icaoType, plane->squawk, plane->latitude, plane->longitude, plane->altitude, plane->heading, plane->speed, plane->verticalSpeed);
        }
        pthread_mutex_unlock(&mutex);

        sleep(1);
    }

    pthread_mutex_destroy(&mutex);
    return 0;
}
