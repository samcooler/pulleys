#pragma once
inline bool valid_calendar_time(int year, int month, int day, int hour, int minute, int second) {
    if (year < 2024 || year > 2099 || month < 1 || month > 12 || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 || second < 0 || second > 59) return false;
    const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int maximum = days[month - 1];
    if (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ++maximum;
    return day >= 1 && day <= maximum;
}
