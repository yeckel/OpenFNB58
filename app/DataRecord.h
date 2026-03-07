#pragma once
#include <QtGlobal>
#include <cmath>

struct DataRecord {
    double timestamp  = 0;   // seconds since session start
    double vbus       = 0;   // V
    double ibus       = 0;   // A
    double power      = 0;   // W
    double dp         = std::numeric_limits<double>::quiet_NaN(); // V
    double dn         = std::numeric_limits<double>::quiet_NaN(); // V
    double temp       = std::numeric_limits<double>::quiet_NaN(); // °C
    double energyCum  = 0;   // Wh, running cumulative sum
};
