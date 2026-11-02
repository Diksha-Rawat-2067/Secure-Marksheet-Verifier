#include "web_server.h"
#include "ocr_engine.h"
#include "verifier.h"
#include "custom_hash.h"

#include <winsock2.h>
#include <windows.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <iomanip>
#include <algorithm>

// JSON string escaping helper
static std::string escapeJson(const std::string& input) {
    std::ostringstream ss;
    for (char c : input) {
        if (c == '"') ss << "\\\"";
        else if (c == '\\') ss << "\\\\";
        else if (c == '\b') ss << "\\b";
        else if (c == '\f') ss << "\\f";
        else if (c == '\n') ss << "\\n";
        else if (c == '\r') ss << "\\r";
        else if (c == '\t') ss << "\\t";
        else if (static_cast<unsigned char>(c) < 32) {
            ss << " ";
        } else {
            ss << c;
        }
    }
    return ss.str();
}

// Lightweight, 100% portable JSON field extractor (zero regex dependency)
static std::string extractJsonField(const std::string& json, const std::string& key) {
    std::string searchKey = "\"" + key + "\"";
    size_t keyPos = json.find(searchKey);
    if (keyPos == std::string::npos) return "";

    size_t colonPos = json.find(':', keyPos + searchKey.length());
    if (colonPos == std::string::npos) return "";

    size_t valStart = json.find_first_not_of(" \t\r\n", colonPos + 1);
    if (valStart == std::string::npos) return "";

    if (json[valStart] == '\"') {
        size_t valEnd = valStart + 1;
        while (valEnd < json.length()) {
            if (json[valEnd] == '\"' && json[valEnd - 1] != '\\') {
                break;
            }
            valEnd++;
        }
        if (valEnd >= json.length()) return "";
        return json.substr(valStart + 1, valEnd - valStart - 1);
    } else {
        size_t valEnd = json.find_first_of(",}\r\n \t", valStart);
        if (valEnd == std::string::npos) valEnd = json.length();
        return json.substr(valStart, valEnd - valStart);
    }
}

// Base64 decoder for receiving uploaded images directly in HTTP POST JSON payloads
static std::vector<unsigned char> base64Decode(const std::string& in) {
    std::vector<unsigned char> out;
    std::vector<int> T(256, -1);
    const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < 64; i++) {
        T[static_cast<unsigned char>(chars[i])] = i;
    }

    int val = 0, valb = -8;
    for (char c : in) {
        if (c == '=') break;
        if (static_cast<unsigned char>(c) >= 256 || T[static_cast<unsigned char>(c)] == -1) continue;
        val = (val << 6) + T[static_cast<unsigned char>(c)];
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<unsigned char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

WebServer::WebServer(MerkleTree& tree, int port)
    : tree_(tree), port_(port), running_(false) {}

WebServer::~WebServer() {
    stop();
}

void WebServer::stop() {
    running_ = false;
}

std::string WebServer::getContentType(const std::string& path) {
    if (path.find(".html") != std::string::npos) return "text/html";
    if (path.find(".css") != std::string::npos) return "text/css";
    if (path.find(".js") != std::string::npos) return "application/javascript";
    if (path.find(".png") != std::string::npos) return "image/png";
    if (path.find(".jpg") != std::string::npos || path.find(".jpeg") != std::string::npos) return "image/jpeg";
    if (path.find(".json") != std::string::npos) return "application/json";
    if (path.find(".csv") != std::string::npos) return "text/csv";
    return "text/plain";
}

void WebServer::sendResponse(uintptr_t clientSocket, int statusCode, const std::string& contentType, const std::string& body) {
    std::string statusText = (statusCode == 200) ? "OK" : 
                            (statusCode == 204 ? "No Content" : 
                            (statusCode == 404 ? "Not Found" : "Bad Request"));
    std::ostringstream ss;
    ss << "HTTP/1.1 " << statusCode << " " << statusText << "\r\n"
       << "Content-Type: " << contentType << "; charset=utf-8\r\n"
       << "Content-Length: " << body.size() << "\r\n"
       << "Access-Control-Allow-Origin: *\r\n"
       << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
       << "Access-Control-Allow-Headers: Content-Type\r\n"
       << "Connection: close\r\n\r\n"
       << body;

    std::string resp = ss.str();
    SOCKET sock = static_cast<SOCKET>(clientSocket);
    int totalSent = 0;
    int toSend = static_cast<int>(resp.size());
    while (totalSent < toSend) {
        int sent = send(sock, resp.c_str() + totalSent, toSend - totalSent, 0);
        if (sent <= 0) break;
        totalSent += sent;
    }
}

void WebServer::sendBinaryResponse(uintptr_t clientSocket, const std::string& contentType, const std::vector<char>& data) {
    std::ostringstream ss;
    ss << "HTTP/1.1 200 OK\r\n"
       << "Content-Type: " << contentType << "\r\n"
       << "Content-Length: " << data.size() << "\r\n"
       << "Access-Control-Allow-Origin: *\r\n"
       << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
       << "Access-Control-Allow-Headers: Content-Type\r\n"
       << "Connection: close\r\n\r\n";

    std::string headers = ss.str();
    SOCKET sock = static_cast<SOCKET>(clientSocket);
    send(sock, headers.c_str(), static_cast<int>(headers.size()), 0);
    if (!data.empty()) {
        int totalSent = 0;
        int toSend = static_cast<int>(data.size());
        while (totalSent < toSend) {
            int sent = send(sock, data.data() + totalSent, toSend - totalSent, 0);
            if (sent <= 0) break;
            totalSent += sent;
        }
    }
}

// REST API Handlers
std::string WebServer::handleApiRecords() {
    std::ostringstream ss;
    ss << "{\"root\":\"" << tree_.getRootHash() << "\",\"records\":[";

    bool firstRec = true;
    for (const auto& pair : tree_.getAllRecords()) {
        const StudentRecord& r = pair.second;
        if (!firstRec) ss << ",";
        firstRec = false;

        ss << "{"
           << "\"roll\":\"" << escapeJson(r.getRollNumber()) << "\","
           << "\"name\":\"" << escapeJson(r.getStudentName()) << "\","
           << "\"branch\":\"" << escapeJson(r.getBranch()) << "\","
           << "\"sem\":" << r.getSemester() << ","
           << "\"total_marks\":" << r.getTotalMarksObtained() << ","
           << "\"max_marks\":" << r.getTotalMaxMarks() << ","
           << "\"percentage\":" << std::fixed << std::setprecision(2) << r.getPercentage() << ","
           << "\"leaf_hash\":\"" << r.computeLeafHash() << "\","
           << "\"subjects\":[";

        bool firstSub = true;
        for (const auto& sPair : r.getSubjects()) {
            const SubjectGrade& sg = sPair.second;
            if (!firstSub) ss << ",";
            firstSub = false;
            ss << "{"
               << "\"code\":\"" << escapeJson(sg.subject_code) << "\","
               << "\"name\":\"" << escapeJson(sg.subject_name) << "\","
               << "\"marks\":" << sg.marks_obtained << ","
               << "\"max\":" << sg.max_marks
               << "}";
        }
        ss << "]}";
    }
    ss << "]}";
    return ss.str();
}

std::string WebServer::handleApiTree() {
    std::ostringstream ss;
    ss << "{\"root\":\"" << tree_.getRootHash() << "\",\"layers\":[";

    const auto& layers = tree_.getLayers();
    for (size_t l = 0; l < layers.size(); ++l) {
        if (l > 0) ss << ",";
        ss << "[";
        for (size_t n = 0; n < layers[l].size(); ++n) {
            if (n > 0) ss << ",";
            const MerkleNode* node = layers[l][n];
            ss << "{"
               << "\"hash\":\"" << node->hash << "\","
               << "\"is_leaf\":" << (node->is_leaf ? "true" : "false") << ","
               << "\"roll\":\"" << escapeJson(node->roll_number) << "\""
               << "}";
        }
        ss << "]";
    }
    ss << "]}";
    return ss.str();
}

std::string WebServer::handleApiAuditLog() {
    std::ifstream file("data/audit_log.csv");
    std::ostringstream ss;
    ss << "{\"logs\":[";

    if (file.is_open()) {
        std::string line;
        bool isHeader = true;
        std::vector<std::string> rows;
        while (std::getline(file, line)) {
            if (line.empty()) continue;
            if (isHeader) { isHeader = false; continue; }
            rows.push_back(line);
        }
        file.close();

        // Output latest first
        bool first = true;
        for (int i = static_cast<int>(rows.size()) - 1; i >= 0; --i) {
            std::stringstream lineStream(rows[i]);
            std::string cell;
            std::vector<std::string> cells;
            while (std::getline(lineStream, cell, ',')) {
                if (cell.size() >= 2 && cell.front() == '"' && cell.back() == '"') {
                    cell = cell.substr(1, cell.size() - 2);
                }
                cells.push_back(cell);
            }
            if (cells.size() >= 7) {
                if (!first) ss << ",";
                first = false;
                std::string imgBase = cells[1];
                size_t slash = imgBase.find_last_of("/\\");
                if (slash != std::string::npos) imgBase = imgBase.substr(slash + 1);

                ss << "{"
                   << "\"timestamp\":\"" << escapeJson(cells[0]) << "\","
                   << "\"image\":\"" << escapeJson(imgBase) << "\","
                   << "\"roll\":\"" << escapeJson(cells[2]) << "\","
                   << "\"name\":\"" << escapeJson(cells[3]) << "\","
                   << "\"verdict\":\"" << escapeJson(cells[4]) << "\","
                   << "\"proof\":\"" << escapeJson(cells[5]) << "\","
                   << "\"tampered_count\":\"" << escapeJson(cells[6]) << "\""
                   << "}";
            }
        }
    }
    ss << "]}";
    return ss.str();
}

std::string WebServer::handleApiVerify(const std::string& jsonBody) {
    std::string base64Str = extractJsonField(jsonBody, "image_base64");
    std::string imagePath = extractJsonField(jsonBody, "image_path");

    // Handle base64 image upload from browser file input / drag-and-drop
    if (!base64Str.empty()) {
        size_t commaPos = base64Str.find(',');
        if (commaPos != std::string::npos) {
            base64Str = base64Str.substr(commaPos + 1);
        }
        std::vector<unsigned char> decoded = base64Decode(base64Str);
        if (!decoded.empty()) {
            std::string uploadPath = "data/samples/uploaded_marksheet.png";
            std::ofstream out(uploadPath, std::ios::binary);
            if (out.is_open()) {
                out.write(reinterpret_cast<const char*>(decoded.data()), decoded.size());
                out.close();
                imagePath = uploadPath;
            }
        }
    }

    // Replace escaped slashes if any
    std::string cleanPath = "";
    for (size_t i = 0; i < imagePath.length(); ++i) {
        if (imagePath[i] == '\\' && i + 1 < imagePath.length() && imagePath[i + 1] == '\\') {
            cleanPath += '/';
            i++;
        } else if (imagePath[i] == '\\') {
            cleanPath += '/';
        } else {
            cleanPath += imagePath[i];
        }
    }
    imagePath = cleanPath;

    // Check if relative path needs data/samples/ prefix
    if (!imagePath.empty() && imagePath.find("data/") == std::string::npos && imagePath.find("data\\") == std::string::npos) {
        std::string samplePath = "data/samples/" + imagePath;
        std::ifstream test(samplePath);
        if (test.good()) {
            imagePath = samplePath;
        }
    }

    if (imagePath.empty()) {
        return "{\"success\":false,\"error\":\"image_path or image_base64 is required\"}";
    }

    OCRResult ocr = OCREngine::scanImage(imagePath);
    VerificationReport report = Verifier::verify(tree_, ocr);
    Verifier::logAuditToCSV("data/audit_log.csv", imagePath, report);

    std::vector<MerkleProofStep> proof;
    if (!report.roll_number.empty()) {
        tree_.generateProof(report.roll_number, proof);
    }

    std::string verdictStr = "UNKNOWN";
    if (report.verdict == VerificationVerdict::AUTHENTIC) verdictStr = "AUTHENTIC";
    else if (report.verdict == VerificationVerdict::TAMPERED) verdictStr = "TAMPERED";
    else if (report.verdict == VerificationVerdict::FORGED_NOT_FOUND) verdictStr = "FORGED_NOT_FOUND";
    else if (report.verdict == VerificationVerdict::OCR_READ_ERROR) verdictStr = "OCR_READ_ERROR";

    std::ostringstream ss;
    ss << "{\"success\":true,\"report\":{"
       << "\"verdict\":\"" << verdictStr << "\","
       << "\"roll\":\"" << escapeJson(report.roll_number) << "\","
       << "\"name\":\"" << escapeJson(report.student_name) << "\","
       << "\"paper_name\":\"" << escapeJson(report.paper_student_name) << "\","
       << "\"official_name\":\"" << escapeJson(report.official_student_name) << "\","
       << "\"name_mismatch\":" << (report.name_mismatch ? "true" : "false") << ","
       << "\"official_root\":\"" << report.official_root_hash << "\","
       << "\"official_leaf\":\"" << report.official_leaf_hash << "\","
       << "\"ocr_leaf\":\"" << report.ocr_leaf_hash << "\","
       << "\"merkle_passed\":" << (report.merkle_proof_passed ? "true" : "false") << ","
       << "\"tampered_count\":" << report.tampered_fields_count << ","
       << "\"summary\":\"" << escapeJson(report.summary_message) << "\","
       << "\"raw_ocr\":\"" << escapeJson(ocr.raw_text) << "\",";

    // Audits array
    ss << "\"audits\":[";
    for (size_t i = 0; i < report.field_audits.size(); ++i) {
        const auto& a = report.field_audits[i];
        if (i > 0) ss << ",";
        ss << "{"
           << "\"code\":\"" << escapeJson(a.subject_code) << "\","
           << "\"name\":\"" << escapeJson(a.subject_name) << "\","
           << "\"paper_marks\":" << (a.physical_ocr_marks >= 0 ? std::to_string(a.physical_ocr_marks) : "\"N/A\"") << ","
           << "\"official_marks\":" << a.official_marks << ","
           << "\"status\":\"" << (a.is_match ? "MATCH" : "TAMPERED") << "\""
           << "}";
    }
    ss << "],";

    // Proof array
    ss << "\"proof\":[";
    for (size_t i = 0; i < proof.size(); ++i) {
        if (i > 0) ss << ",";
        ss << "{"
           << "\"level\":" << (i + 1) << ","
           << "\"direction\":\"" << (proof[i].direction == ProofDirection::LEFT ? "LEFT" : "RIGHT") << "\","
           << "\"sibling_hash\":\"" << proof[i].sibling_hash << "\""
           << "}";
    }
    ss << "]}}";

    return ss.str();
}

std::string WebServer::handleApiAddStudent(const std::string& jsonBody) {
    std::string roll = extractJsonField(jsonBody, "roll");
    std::string name = extractJsonField(jsonBody, "name");
    std::string branch = extractJsonField(jsonBody, "branch");
    std::string semStr = extractJsonField(jsonBody, "sem");

    if (branch.empty()) branch = "CSE";
    int sem = 3;
    if (!semStr.empty()) {
        try { sem = std::stoi(semStr); } catch (...) {}
    }

    if (roll.empty() || name.empty()) {
        return "{\"success\":false,\"error\":\"Roll number and name are required.\"}";
    }

    StudentRecord newRec(roll, name, branch, sem);

    // Parse subjects array from JSON: "subjects": [ {"code":"...", "name":"...", "marks":...}, ... ]
    size_t subArrPos = jsonBody.find("\"subjects\"");
    if (subArrPos != std::string::npos) {
        size_t startBracket = jsonBody.find('[', subArrPos);
        size_t endBracket = jsonBody.find(']', startBracket);
        if (startBracket != std::string::npos && endBracket != std::string::npos) {
            std::string subArrStr = jsonBody.substr(startBracket, endBracket - startBracket + 1);
            size_t objStart = 0;
            while ((objStart = subArrStr.find('{', objStart)) != std::string::npos) {
                size_t objEnd = subArrStr.find('}', objStart);
                if (objEnd == std::string::npos) break;
                std::string objStr = subArrStr.substr(objStart, objEnd - objStart + 1);

                std::string code = extractJsonField(objStr, "code");
                std::string subName = extractJsonField(objStr, "name");
                std::string marksStr = extractJsonField(objStr, "marks");
                std::string maxStr = extractJsonField(objStr, "max");

                int marks = 0;
                int maxMarks = 100;
                try { if (!marksStr.empty()) marks = std::stoi(marksStr); } catch (...) {}
                try { if (!maxStr.empty()) maxMarks = std::stoi(maxStr); } catch (...) {}

                if (!code.empty()) {
                    newRec.addSubject(code, subName.empty() ? code : subName, marks, maxMarks);
                }
                objStart = objEnd + 1;
            }
        }
    }

    // Default sample subjects if none provided
    if (newRec.getSubjects().empty()) {
        newRec.addSubject("TCS-301", "Data Structures", 85, 100);
        newRec.addSubject("TCS-302", "OOP with C++", 88, 100);
        newRec.addSubject("TMA-301", "Discrete Maths", 80, 100);
        newRec.addSubject("TCS-304", "Computer Org", 82, 100);
    }

    std::vector<StudentRecord> records;
    for (const auto& pair : tree_.getAllRecords()) {
        records.push_back(pair.second);
    }
    records.push_back(newRec);

    tree_.buildTree(records);
    tree_.exportToCSV("data/university_records.csv");
    tree_.exportLedgerToCSV("data/merkle_ledger.csv");

    return "{\"success\":true,\"root\":\"" + tree_.getRootHash() + "\"}";
}

void WebServer::handleClient(uintptr_t clientSocket) {
    SOCKET sock = static_cast<SOCKET>(clientSocket);

    // Set socket receive and send timeouts (15 seconds) so it never hangs indefinitely
    DWORD rcvTimeout = 15000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&rcvTimeout), sizeof(rcvTimeout));
    DWORD sndTimeout = 15000;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&sndTimeout), sizeof(sndTimeout));

    std::vector<char> buffer;
    buffer.reserve(131072);
    char chunk[16384];
    int totalBytes = 0;
    int expectedLength = -1;
    size_t headerEnd = std::string::npos;

    while (true) {
        int bytes = recv(sock, chunk, sizeof(chunk), 0);
        if (bytes <= 0) break;
        buffer.insert(buffer.end(), chunk, chunk + bytes);
        totalBytes += bytes;

        if (headerEnd == std::string::npos) {
            std::string current(buffer.begin(), buffer.end());
            headerEnd = current.find("\r\n\r\n");
            if (headerEnd != std::string::npos) {
                // Find Content-Length case-insensitively
                std::string lower = current;
                std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                size_t clPos = lower.find("content-length:");
                if (clPos != std::string::npos && clPos < headerEnd) {
                    size_t numStart = current.find_first_of("0123456789", clPos);
                    if (numStart != std::string::npos && numStart < headerEnd) {
                        size_t numEnd = current.find_first_not_of("0123456789", numStart);
                        std::string clVal = current.substr(numStart, numEnd - numStart);
                        try {
                            int cl = std::stoi(clVal);
                            expectedLength = static_cast<int>(headerEnd) + 4 + cl;
                        } catch (...) {}
                    }
                } else {
                    expectedLength = static_cast<int>(headerEnd) + 4;
                }
            }
        }

        if (expectedLength > 0 && totalBytes >= expectedLength) {
            break;
        }
    }

    if (buffer.empty()) {
        closesocket(sock);
        return;
    }

    std::string request(buffer.begin(), buffer.end());
    std::istringstream reqStream(request);
    std::string method, fullPath, protocol;
    reqStream >> method >> fullPath >> protocol;

    // Strip query string if present
    std::string path = fullPath;
    size_t qPos = path.find('?');
    if (qPos != std::string::npos) {
        path = path.substr(0, qPos);
    }

    // Split body for POST
    std::string body = "";
    if (headerEnd != std::string::npos && headerEnd + 4 < buffer.size()) {
        body = std::string(buffer.begin() + headerEnd + 4, buffer.end());
    }

    // CORS preflight
    if (method == "OPTIONS") {
        sendResponse(clientSocket, 200, "text/plain", "");
        shutdown(sock, SD_SEND);
        closesocket(sock);
        return;
    }

    if (method == "GET") {
        if (path == "/" || path == "/index.html") {
            std::ifstream f("frontend/index.html");
            if (f.is_open()) {
                std::stringstream ss;
                ss << f.rdbuf();
                sendResponse(clientSocket, 200, "text/html", ss.str());
            } else {
                sendResponse(clientSocket, 404, "text/plain", "frontend/index.html not found");
            }
        } else if (path == "/style.css" || path == "/frontend/style.css") {
            std::ifstream f("frontend/style.css");
            if (f.is_open()) {
                std::stringstream ss;
                ss << f.rdbuf();
                sendResponse(clientSocket, 200, "text/css", ss.str());
            } else {
                sendResponse(clientSocket, 404, "text/plain", "style.css not found");
            }
        } else if (path == "/app.js" || path == "/frontend/app.js") {
            std::ifstream f("frontend/app.js");
            if (f.is_open()) {
                std::stringstream ss;
                ss << f.rdbuf();
                sendResponse(clientSocket, 200, "application/javascript", ss.str());
            } else {
                sendResponse(clientSocket, 404, "text/plain", "app.js not found");
            }
        } else if (path == "/favicon.ico") {
            sendResponse(clientSocket, 204, "image/x-icon", "");
        } else if (path.rfind("/samples/", 0) == 0) {
            std::string filename = path.substr(9);
            std::string fullPathOnDisk = "data/samples/" + filename;
            std::ifstream f(fullPathOnDisk, std::ios::binary);
            if (f.is_open()) {
                std::vector<char> imgData((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                sendBinaryResponse(clientSocket, "image/png", imgData);
            } else {
                sendResponse(clientSocket, 404, "text/plain", "Sample image not found: " + fullPathOnDisk);
            }
        } else if (path.rfind("/data/samples/", 0) == 0) {
            std::string filename = path.substr(14);
            std::string fullPathOnDisk = "data/samples/" + filename;
            std::ifstream f(fullPathOnDisk, std::ios::binary);
            if (f.is_open()) {
                std::vector<char> imgData((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                sendBinaryResponse(clientSocket, "image/png", imgData);
            } else {
                sendResponse(clientSocket, 404, "text/plain", "Sample image not found");
            }
        } else if (path == "/data/university_records.csv" || path == "/export/records.csv") {
            std::ifstream f("data/university_records.csv");
            if (f.is_open()) {
                std::stringstream ss;
                ss << f.rdbuf();
                sendResponse(clientSocket, 200, "text/csv", ss.str());
            } else {
                sendResponse(clientSocket, 404, "text/plain", "university_records.csv not found");
            }
        } else if (path == "/data/merkle_ledger.csv" || path == "/export/ledger.csv") {
            std::ifstream f("data/merkle_ledger.csv");
            if (f.is_open()) {
                std::stringstream ss;
                ss << f.rdbuf();
                sendResponse(clientSocket, 200, "text/csv", ss.str());
            } else {
                sendResponse(clientSocket, 404, "text/plain", "merkle_ledger.csv not found");
            }
        } else if (path == "/data/audit_log.csv" || path == "/export/audit.csv") {
            std::ifstream f("data/audit_log.csv");
            if (f.is_open()) {
                std::stringstream ss;
                ss << f.rdbuf();
                sendResponse(clientSocket, 200, "text/csv", ss.str());
            } else {
                sendResponse(clientSocket, 404, "text/plain", "audit_log.csv not found");
            }
        } else if (path == "/api/records") {
            sendResponse(clientSocket, 200, "application/json", handleApiRecords());
        } else if (path == "/api/tree") {
            sendResponse(clientSocket, 200, "application/json", handleApiTree());
        } else if (path == "/api/audit-log") {
            sendResponse(clientSocket, 200, "application/json", handleApiAuditLog());
        } else {
            sendResponse(clientSocket, 404, "text/plain", "Route not found");
        }
    } else if (method == "POST") {
        if (path == "/api/verify") {
            sendResponse(clientSocket, 200, "application/json", handleApiVerify(body));
        } else if (path == "/api/add-student") {
            sendResponse(clientSocket, 200, "application/json", handleApiAddStudent(body));
        } else {
            sendResponse(clientSocket, 404, "text/plain", "POST endpoint not found");
        }
    }

    // Graceful TCP shutdown: announce we are done sending
    shutdown(sock, SD_SEND);

    // Read and discard any trailing client bytes so TCP connection finishes without RST
    char drain[1024];
    while (recv(sock, drain, sizeof(drain), 0) > 0) {}

    closesocket(sock);
}

void WebServer::run() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "[WebServer] WSAStartup failed.\n";
        return;
    }

    SOCKET serverSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (serverSocket == INVALID_SOCKET) {
        std::cerr << "[WebServer] Socket creation failed.\n";
        WSACleanup();
        return;
    }

    int opt = 1;
    setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(static_cast<u_short>(port_));

    if (bind(serverSocket, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cerr << "[WebServer] Socket bind failed on port " << port_ << ".\n";
        closesocket(serverSocket);
        WSACleanup();
        return;
    }

    if (listen(serverSocket, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "[WebServer] Socket listen failed.\n";
        closesocket(serverSocket);
        WSACleanup();
        return;
    }

    running_ = true;
    std::cout << "\n======================================================================\n";
    std::cout << "         PURE C++ EMBEDDED WEB SERVER ACTIVE (WINSOCK2)               \n";
    std::cout << "         PORT: " << port_ << " | URL: http://localhost:" << port_ << "\n";
    std::cout << "         Press Ctrl+C in console to stop the server                   \n";
    std::cout << "======================================================================\n";

    // Auto-open default browser
    std::string openCmd = "start http://localhost:" + std::to_string(port_);
    system(openCmd.c_str());

    while (running_) {
        sockaddr_in clientAddr;
        int clientAddrLen = sizeof(clientAddr);
        SOCKET clientSocket = accept(serverSocket, reinterpret_cast<sockaddr*>(&clientAddr), &clientAddrLen);
        if (clientSocket != INVALID_SOCKET) {
            handleClient(static_cast<uintptr_t>(clientSocket));
        }
    }

    closesocket(serverSocket);
    WSACleanup();
}

static DWORD WINAPI WebServerThreadFunc(LPVOID param) {
    WebServer* server = static_cast<WebServer*>(param);
    server->run();
    return 0;
}

void WebServer::startAsync() {
    CreateThread(NULL, 0, WebServerThreadFunc, this, 0, NULL);
}
