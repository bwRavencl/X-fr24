#include "parson/parson.h"

#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>

// define maximum number of planes
#define MAX_PLANES 4096

#define DATA_INDEX_LATITUDE 1
#define DATA_INDEX_LONGITUDE 2
#define DATA_INDEX_HEADING 3
#define DATA_INDEX_ALTITUDE 4
#define DATA_INDEX_SPEED 5
#define DATA_INDEX_REGISTRATION 9
#define DATA_INDEX_VERTICAL_SPEED 16
#define DATA_INDEX_ICAO 17

static int planeCount = 0;
static struct Plane* planes[MAX_PLANES] = {NULL};

struct url_data
{
    size_t size;
    char* data;
};

static size_t WriteData(void *ptr, size_t size, size_t nmemb, url_data *data)
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
    
    url_data data;
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
printf("Data Size = %d\n", (int) data.size);
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

    char *balance = GetUrl("http://www.flightradar24.com/balance.json");
    if (balance != NULL)
    {
        JSON_Value *rootJson = json_parse_string(balance);
        free(balance);

        if (rootJson != NULL)
        {
            if (rootJson->type == JSONObject)
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

                            if (load != 0 && (bestUrl == NULL || load < bestLoad))
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
            }

            json_value_free(rootJson);
        }
    }

    return bestUrl;
}

static char* ParseZones(JSON_Object *zonesJson, double latitude, double longitude)
{
printf("Subcall!\n");
    char *bestZone = NULL;

                if (zonesJson != NULL)
                {
                    size_t zoneCount = json_object_get_count(zonesJson);
printf("zoneCount = %d\n", (int) zoneCount);

                    for (int i = 0; i < zoneCount; i++)
                    {
                        const char *z = json_object_get_name(zonesJson, i);
printf("zone = %s\n", z);
                        JSON_Value *valueJson = json_object_get_value(zonesJson, z);

                        if (valueJson->type == JSONObject)
                        {
                            JSON_Object *zoneJson = json_value_get_object(valueJson);
                            double tl_x = 0.0, tl_y = 0.0, br_x = 0.0, br_y = 0.0;
                            JSON_Object *subzonesJson = NULL;

                            size_t propertyCount = json_object_get_count(zoneJson);
                            for (int j = 0; j < propertyCount; j++)
                            {
                                const char *p = json_object_get_name(zoneJson, j);
                                JSON_Value *propertyJson = json_object_get_value(zoneJson, p);

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
                                else if (propertyJson->type == JSONObject)
                                {
                                    if (strcmp(p, "subzones") == 0)
                                        subzonesJson = json_value_get_object(propertyJson);
                                }
                            }
//printf(" tl_y = %f\n tl_x =%f\n br_y = %f\n br_x = %f\n", tl_y, tl_x, br_y, br_x);

                            if (tl_x != 0.0 && tl_y != 0.0 && br_x != 0.0 && br_y != 0.0)
                            {
                                if (longitude >= tl_x && latitude <= tl_y && longitude <= br_x && latitude >= br_y)
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
                }
return bestZone;
}

static char* GetZone(double latitude, double longitude)
{
    char *zoneName = NULL;
    char *zones = GetUrl("http://www.flightradar24.com/js/zones.js.php");

    if (zones != NULL)
    {
        JSON_Value *rootJson = json_parse_string(zones);
        free(zones);

        if (rootJson->type == JSONObject)
            zoneName = ParseZones(json_value_get_object(rootJson), latitude, longitude);

        json_value_free(rootJson);
    }

    return zoneName;
}

// plane struct
struct Plane
{
    char icao[8];
    char registration[8];
    double latitude; // degrees
    double longitude; // degrees
    double altitude; // feet MSL
    float pitch; // degrees
    float roll; // degrees
    float heading; // degrees
    int speed; // knots
    int verticalSpeed; // feet per minute
};

void UpdateAircraft(char *balancerUrl, char *zone)
{
    #define URL_PART_MID "/zones/fcgi/"
    #define URL_PART_END "_all.json"

    for (int i = 0; i < planeCount; i++)
    {
        if (planes[i] != NULL)
            free(planes[i]);
    }
    planeCount = 0;

    char url[strlen(balancerUrl) + strlen(URL_PART_MID) + strlen(zone) + strlen(URL_PART_END)];
    sprintf(url, "%s%s%s%s", balancerUrl, URL_PART_MID, zone, URL_PART_END);
    printf("Getting: %s\n", url);

    char *data = GetUrl(url);
//    printf("%s", data);
    if (data != NULL)
    {
        JSON_Value *rootJson = json_parse_string(data);
        free(data);
        if (rootJson != NULL)
        {
            if (rootJson->type == JSONObject)
            {
                JSON_Object *aircraftJson = json_value_get_object(rootJson);
                if (aircraftJson != NULL)
                {
                    size_t aircraftCount = json_object_get_count(aircraftJson);
printf("aircraftCount = %d\n", (int) aircraftCount);
                    for (int i = 0; i < aircraftCount; i++)
                    {
                        const char *id = json_object_get_name(aircraftJson, i);
                        JSON_Value *value = json_object_get_value(aircraftJson, id);

                        if (value != NULL && value->type == JSONArray)
                        {
                            JSON_Array *dataJson = json_value_get_array(value);

                            if (dataJson != NULL)
                            {           const char *icao = NULL, *registration = NULL;
                                        int speed = 0, verticalSpeed = 0;
                                        float heading = 0.0f;
                                        double latitude = 0.0, longitude = 0.0, altitude = 0.0;


                                int dataCount = json_array_get_count(dataJson);
                                for (int k = 0; k < dataCount; k++)
                                {
                                        JSON_Value *valueJson = json_array_get_value(dataJson, k);
                                        if (valueJson != NULL)
                                        {
                                        switch (k)
                                        {
                                            case DATA_INDEX_LATITUDE:
                                                latitude = json_value_get_number(valueJson);
                                                break;
                                            case DATA_INDEX_LONGITUDE:
                                                longitude = json_value_get_number(valueJson);
                                                break;
                                            case DATA_INDEX_ALTITUDE:
                                                altitude = json_value_get_number(valueJson);
                                                break;
                                            case DATA_INDEX_HEADING:
                                                heading = (float) json_value_get_number(valueJson);
                                                break;
                                            case DATA_INDEX_SPEED:
                                                speed = (int) json_value_get_number(valueJson);
                                                break;
                                            case DATA_INDEX_VERTICAL_SPEED:
                                                verticalSpeed = (int) json_value_get_number(valueJson);
                                                break;
                                            case DATA_INDEX_REGISTRATION:
                                                registration = json_value_get_string(valueJson);
                                                break;
                                            case DATA_INDEX_ICAO:
                                                icao = json_value_get_string(valueJson);
                                                break;
					}
                                        }
                                 }

                                        if (planeCount < MAX_PLANES && latitude != 0.0 && longitude != 0.0 && altitude != 0.0)
                                        {
                                            Plane *p = (Plane*) malloc(sizeof(*p));
                                            if (p != NULL)
                                            {
                                            if (registration != NULL)
                                                strncpy(p->registration, registration, 8);
                                            else
                                                strncpy(p->registration, "Unknown", 8);

                                            if (icao != NULL)
                                                strncpy(p->icao, icao, 8);
                                            else
                                                strncpy(p->icao, "Unknown", 8);
                                            p->latitude = latitude;
                                            p->longitude = longitude;
                                            p->altitude = altitude;
                                            p->pitch = 0.0f;
                                            p->roll = 0.0f;
                                            p->heading = heading;
                                            p->speed = speed;
                                            p->verticalSpeed = verticalSpeed;

                                            planes[planeCount] = p;
                                            planeCount++;
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

int main(void)
{
    char *balancerUrl = GetBalancerUrl();
    printf("-> Selected Balancer URL = %s\n", balancerUrl);

    #define LON 51.5286416,-0.1015987
    #define EDB 55.9410655,-3.2053836
    #define MUC 48.1549107,11.5418357
    #define SFO 37.7577,-122.4376
    #define MLB -36.0491656,147.1498521
    #define WSW 52.232938,21.0611941


    char *zone = GetZone(MUC);
    printf("-> Selected Zone = %s\n", zone);
while(true)
{
    UpdateAircraft(balancerUrl, zone);

    for (int i = 0; i < planeCount; i++)
    {
        Plane *p = planes[i];
        printf("Plane %d:\n ICAO = %s\n Registration = %s\n Latitude = %f\n Longitude = %f\n Altitude = %f\n Speed = %d\n Vertical Speed = %d\n Heading = %f\n\n", i, p->icao, p->registration, p->latitude, p->longitude, p->altitude, p->speed, p->verticalSpeed, p->heading);
    }
}

    free(balancerUrl);
    free(zone);
}
