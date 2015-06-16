#include "parson/parson.h"

#include <curl/curl.h>
#include <map>
#include <math.h>
#include <stdlib.h>
#include <pthread.h>
#include <string>
#include <string.h>
#include <unistd.h>

// define maximum number of planes
#define MAX_PLANES 4096

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
#define ARRAY_INDEX_REGISTRATION 9
#define ARRAY_INDEX_VERTICAL_SPEED 16
#define ARRAY_INDEX_ICAO 17

// define radius of the earth in nautical miles
#define RADIUS_EARTH 3440.07

// plane struct
struct Plane
{
    char icao[8];
    char registration[8];
    double latitude; // degrees
    double longitude; // degrees
    double altitude; // feet AGL
    float pitch; // degrees
    float roll; // degrees
    float heading; // degrees
    int speed; // knots
    int verticalSpeed; // feet per minute
    time_t lastSeen; // seconds
};

//static double latitude = 0.0, longitude = 0.0;
//static double latitude = 51.5286416 , longitude = -0.1015987; // London
static double myLatitude = 48.3537449, myLongitude = 11.7860028; // Munich
//static double myLatitude = 50.9987326, myLongitude = 13.9867738; // Desden
//static double myLatitude = 37.7577, myLongitude = -122.4376; // San Francisco
std::map<std::string, Plane*> planes;

struct UrlData
{
    size_t size;
    char* data;
};

static size_t WriteData(void *ptr, size_t size, size_t nmemb, UrlData *data)
{
    size_t index = data->size;
    size_t n = (size * nmemb);
    char* tmp;
    
    data->size += (size * nmemb);
    
    tmp = (char*) realloc(data->data, data->size + 1); /* +1 for '\0' */
    
    if(tmp)
        data->data = tmp;
    else
    {
        if(data->data)
            free(data->data);
        
        return 0;
    }
    
    memcpy((data->data + index), ptr, n);
    data->data[data->size] = '\0';
    
    return size * nmemb;
}

static char* GetUrl(const char* url)
{
    CURL *curl = NULL;
    
    UrlData data;
    data.size = 0;
    data.data = (char*) malloc(4096);
    if(NULL == data.data)
        return NULL;
    
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

    return data.data;
}

typedef union json_value_value
{
    char *string;
    double number;
    JSON_Object *object;
    JSON_Array *array;
    int boolean;
    int null;
} JSON_Value_Value;

struct json_value_t
{
    JSON_Value_Type type;
    JSON_Value_Value value;
};

static char* GetBalancerUrl(void)
{
    char *bestUrl = NULL;

    char *balance = GetUrl(URL_BALANCE);
    if (balance != NULL)
    {
        JSON_Value *rootJson = json_parse_string(balance);
        free(balance);

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
                                free(bestUrl);

                            bestUrl = (char*) malloc(1 + strlen(url));
                            if (bestUrl != NULL)
                                strcpy(bestUrl, url);

                            bestLoad = load;
                        }
                    }
                }
            }

            json_value_free(rootJson);
        }
    }

    return bestUrl;
}

static char* ParseZones(JSON_Object *zonesJson, double latitude, double longitude)
{
    char *bestZone = NULL;

    if (zonesJson != NULL)
    {
        size_t zoneCount = json_object_get_count(zonesJson);

        for (int i = 0; i < zoneCount; i++)
        {
            const char *z = json_object_get_name(zonesJson, i);
            JSON_Value *valueJson = json_object_get_value(zonesJson, z);

            if (valueJson != NULL && valueJson->type == JSONObject)
            {
                JSON_Object *zoneJson = json_value_get_object(valueJson);
                double tl_x = 0.0, tl_y = 0.0, br_x = 0.0, br_y = 0.0;
                JSON_Object *subzonesJson = NULL;

                size_t propertyCount = json_object_get_count(zoneJson);
                for (int j = 0; j < propertyCount; j++)
                {
                    const char *p = json_object_get_name(zoneJson, j);
                    JSON_Value *propertyJson = json_object_get_value(zoneJson, p);

                    if (propertyJson != NULL)
                    {
                        if (propertyJson->type == JSONNumber)
                        {
                            double number = json_value_get_number(propertyJson);

                            if (strcmp(p, "tl_x") == 0)
                                tl_x = number;
                            else if (strcmp(p, "tl_y") == 0)
                                tl_y = number;
                            else if (strcmp(p, "br_x") == 0)
                                br_x = number;
                            else if (strcmp(p, "br_y") == 0)
                                br_y = number;
                        }
                        else if (propertyJson->type == JSONObject && (strcmp(p, "subzones") == 0))
                                subzonesJson = json_value_get_object(propertyJson);
                    }
                }

                    if (tl_x != 0.0 && tl_y != 0.0 && br_x != 0.0 && br_y != 0.0 && longitude > tl_x + 2 && latitude < tl_y - 2 && longitude < br_x - 2 && latitude > br_y + 2)
                    {
                        if (subzonesJson != NULL)
                            bestZone = ParseZones(subzonesJson, latitude, longitude);

                        if (bestZone == NULL)
                        {
                            bestZone = (char*) malloc(1 + strlen(z));
                            if (bestZone != NULL)
                                strcpy(bestZone, z);
                        }

                        break;
                    }
                }
            }
        }

    return bestZone;
}

static char* GetZoneName(double latitude, double longitude)
{
    char *zoneName = NULL;
    char *zones = GetUrl(URL_ZONES);

    if (zones != NULL)
    {
        JSON_Value *rootJson = json_parse_string(zones);
        free(zones);

        if (rootJson != NULL)
        {
            if (rootJson->type == JSONObject)
                zoneName = ParseZones(json_value_get_object(rootJson), latitude, longitude);

            json_value_free(rootJson);
        }
    }

    return zoneName;
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

double Distance(double latitude1, double longitude1, double latitude2, double longitude2)
{
    double dLatitude = latitude2 - latitude1;
    double dLongitude = longitude2 - longitude1;

    double a = pow(sin(DegreesToRadians(dLatitude / 2.0)), 2) + cos(DegreesToRadians(latitude1)) * cos(DegreesToRadians(latitude2)) * pow(sin(DegreesToRadians(dLongitude / 2.0)), 2);
    double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));

    return c * RADIUS_EARTH;
}

void UpdatePlanes(char *balancerUrl, char *zoneName)
{
    time_t currentTime = time(NULL);

    if (currentTime != ((time_t) -1))
    {
    for (std::map<std::string, Plane*>::iterator p = planes.begin(); p != planes.end(); ++p)
    {
        Plane *plane = p->second;
        if (currentTime - plane->lastSeen > PLANE_TIMEOUT)
        {
            printf("Removing: %s - CurrentTime = %d - LastSeen = %d\n", p->first.c_str(), (int) currentTime, (int) plane->lastSeen);
            free(plane);
            planes.erase(p->first);
        }
    }

    char url[strlen(balancerUrl) + strlen(URL_ZONE_INFIX) + strlen(zoneName) + strlen(URL_ZONE_SUFFIX)];
    sprintf(url, "%s%s%s%s", balancerUrl, URL_ZONE_INFIX, zoneName, URL_ZONE_SUFFIX);

    char *zone = GetUrl(url);
    if (zone != NULL)
    {
        JSON_Value *rootJson = json_parse_string(zone);
        free(zone);
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
                                const char *icao = NULL, *registration = NULL;
                                int speed = 0, verticalSpeed = 0;
                                float heading = 0.0f;
                                double latitude = 0.0, longitude = 0.0, altitude = 0.0;

                                int propertyCount = json_array_get_count(propertiesJson);
                                for (int k = 0; k < propertyCount; k++)
                                {
                                    JSON_Value *valueJson = json_array_get_value(propertiesJson, k);

                                    if (valueJson != NULL)
                                    {
                                        switch (k)
                                        {
                                            case ARRAY_INDEX_LATITUDE:
                                                latitude = json_value_get_number(valueJson);
                                                break;
                                            case ARRAY_INDEX_LONGITUDE:
                                                longitude = json_value_get_number(valueJson);
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
                                            case ARRAY_INDEX_VERTICAL_SPEED:
                                                verticalSpeed = (int) json_value_get_number(valueJson);
                                                break;
                                            case ARRAY_INDEX_REGISTRATION:
                                                registration = json_value_get_string(valueJson);
                                                break;
                                            case ARRAY_INDEX_ICAO:
                                                icao = json_value_get_string(valueJson);
                                                break;
                                        }
                                    }
                                }

                                if (planes.size() < MAX_PLANES && latitude != 0.0 && longitude != 0.0 && Distance(myLatitude, myLongitude, latitude, longitude) <= MAX_DISTANCE)
                                {
                                    Plane *plane = NULL;

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
                                        if (registration != NULL && strlen(registration) > 0)
                                            strncpy(plane->registration, registration, 8);
                                        else
                                            strncpy(plane->registration, "Unknown", 8);

                                        if (icao != NULL && strlen(icao) > 0)
                                            strncpy(plane->icao, icao, 8);
                                        else
                                            strncpy(plane->icao, "Unknown", 8);

                                        plane->latitude = latitude;
                                        plane->longitude = longitude;
                                        plane->altitude = altitude;
                                        plane->pitch = 0.0f;
                                        plane->roll = 0.0f;
                                        plane->heading = heading;
                                        plane->speed = speed;
                                        plane->verticalSpeed = verticalSpeed;
                                        plane->lastSeen = currentTime;
                                    }
                                }
                            }
                        }
                    }
                }
            }

            json_value_free(rootJson);
        }
    }
}
}
}

void *ThreadFunction(void *ptr)
{
    srand(time(NULL));

while (true)
{

    char *balancerUrl = GetBalancerUrl();
//printf("URL -> %s\n", balancerUrl);
    if (balancerUrl != NULL)
    {
        char *zoneName = GetZoneName(myLatitude, myLongitude);
        if (zoneName != NULL)
        {
//printf("ZONE -> %s\n", zoneName);
            UpdatePlanes(balancerUrl, zoneName);
            free(zoneName);
        }

        free(balancerUrl);
    }

    sleep(1);
}
}

int main(void)
{
    pthread_t thread = 0;
    pthread_create(&thread, NULL, ThreadFunction, NULL);

int frame = 0;
while(true)
{
    printf("\033[2J\033[1;1H");
    printf("Frame = %d PlaneCount = %d\n\n", frame, (int) planes.size());
    frame++;
    for (std::map<std::string, Plane*>::iterator p = planes.begin(); p != planes.end(); ++p)
    {
        Plane *plane = p->second;
            printf("Plane %s:\n ICAO = %s\n Registration = %s\n Latitude = %f\n Longitude = %f\n Altitude = %f\n Speed = %d\n Vertical Speed = %d\n Heading = %f\n\n", p->first.c_str(), plane->icao, plane->registration, plane->latitude, plane->longitude, plane->altitude, plane->speed, plane->verticalSpeed, plane->heading);
    }
    sleep(1);
}

    return 0;
}
