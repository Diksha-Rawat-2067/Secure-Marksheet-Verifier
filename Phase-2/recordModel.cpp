#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <iomanip>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <unordered_map>

struct StudentRecord {
    std::string student_id;
    std::string roll_no;
    std::string name;
    std::string department;
    int semester;
    std::string section;
    // Academic Subject Marks from CSV
    int math;
    int programming;
    int dbms;
    int networks;
    int os;
    int communication;
    int total_marks;
    double percentage;
    std::string grade;
    std::string result;
    StudentRecord() : semester(3), math(0), programming(0), dbms(0),
                      networks(0), os(0), communication(0),
                      total_marks(0), percentage(0.0) {}
    // Canonical representation for deterministic hashing
    std::string getCanonicalString() const {
        std::ostringstream ss;
        ss << "ID:" << student_id << "|"
           << "ROLL:" << roll_no << "|"
           << "NAME:" << name << "|"
           << "DEPT:" << department << "|"
           << "SEM:" << semester << "|"
           << "SEC:" << section << "|"
           << "COMM:" << communication << "|"
           << "CN:" << networks << "|"
           << "DBMS:" << dbms << "|"
           << "MATH:" << math << "|"
           << "OS:" << os << "|"
           << "PROG:" << programming;
        return ss.str();
    }
    std::string computeLeafHash() const {
        return CustomHash::hash(getCanonicalString());
    }
};
