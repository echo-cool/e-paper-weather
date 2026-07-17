#ifndef __LOCATION_H__
#define __LOCATION_H__

#include <Arduino.h>

void loadLocationSettings();
bool setLocationFromQuery(const String &query, String &message);

#endif
