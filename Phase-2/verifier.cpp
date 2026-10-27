#include "verifier.h"
#include <iomanip>
#include <fstream>
#include <ctime>

Verifier::Verifier() {}

VerificationReport Verifier::verify(const MerkleTree& universityTree, const OCRResult& ocrResult) {
    VerificationReport report;
    report.official_root_hash = universityTree.getRootHash();

    if (!ocrResult.success) {
        report.verdict = VerificationVerdict::OCR_READ_ERROR;
        report.summary_message = "OCR Error: " + ocrResult.error_message;
        return report;
    }

    const StudentRecord& ocrRec = ocrResult.extracted_record;
    report.roll_number = ocrRec.getRollNumber();
    report.paper_student_name = ocrRec.getStudentName();
    report.student_name = ocrRec.getStudentName();

    // 1. Check if student exists in official database
    StudentRecord officialRec;
    if (!universityTree.getStudentRecord(report.roll_number, officialRec)) {
        report.verdict = VerificationVerdict::FORGED_NOT_FOUND;
        report.summary_message = "FORGERY DETECTED: Roll number '" + report.roll_number + 
                                "' is not registered in the University Database!";
        report.ocr_leaf_hash = ocrRec.computeLeafHash();
        return report;
    }

    report.official_student_name = officialRec.getStudentName();
    report.student_name = officialRec.getStudentName(); // Official verified name
    report.official_leaf_hash = officialRec.computeLeafHash();

    // Check for Name / Identity Discrepancy
    if (!report.paper_student_name.empty() && 
        report.paper_student_name != "Verified Candidate" && 
        report.paper_student_name != report.official_student_name) {
        report.name_mismatch = true;
    }

    // Build canonical record using official subject names/branches but with OCR marks
    StudentRecord normalizedOCR(officialRec.getRollNumber(), officialRec.getStudentName(), 
                                officialRec.getBranch(), officialRec.getSemester());

    const auto& officialSubs = officialRec.getSubjects();
    const auto& ocrSubs = ocrRec.getSubjects();

    for (const auto& pair : officialSubs) {
        const std::string& code = pair.first;
        const SubjectGrade& offGrade = pair.second;

        FieldAudit audit;
        audit.subject_code = code;
        audit.subject_name = offGrade.subject_name;
        audit.official_marks = offGrade.marks_obtained;

        SubjectGrade ocrGrade;
        if (ocrRec.getSubject(code, ocrGrade)) {
            audit.physical_ocr_marks = ocrGrade.marks_obtained;
        } else {
            audit.physical_ocr_marks = -1;
        }

        audit.is_match = (audit.physical_ocr_marks == audit.official_marks);
        if (!audit.is_match) {
            report.tampered_fields_count++;
        }
        report.field_audits.push_back(audit);

        normalizedOCR.addSubject(code, offGrade.subject_name, 
                                (audit.physical_ocr_marks >= 0 ? audit.physical_ocr_marks : 0), 
                                offGrade.max_marks);
    }

    report.ocr_leaf_hash = normalizedOCR.computeLeafHash();

    // 2. Merkle Cryptographic Proof Verification
    std::vector<MerkleProofStep> proof;
    if (universityTree.generateProof(report.roll_number, proof)) {
        report.merkle_proof_passed = MerkleTree::verifyProof(
            report.ocr_leaf_hash, 
            proof, 
            report.official_root_hash
        );
    } else {
        report.merkle_proof_passed = false;
    }

    // 3. Set Final Verdict
    if (report.merkle_proof_passed && report.tampered_fields_count == 0 && !report.name_mismatch) {
        report.verdict = VerificationVerdict::AUTHENTIC;
        report.summary_message = "100% AUTHENTIC: Cryptographic Merkle Proof Validated & All Marks Match University Records.";
    } else {
        report.verdict = VerificationVerdict::TAMPERED;
        std::string msg = "";
        if (report.name_mismatch) {
            msg += "IDENTITY FRAUD DETECTED: Marksheet states '" + report.paper_student_name + 
                   "', but Roll " + report.roll_number + " belongs to '" + report.official_student_name + "'! ";
        }
        if (report.tampered_fields_count > 0) {
            msg += "TAMPER DETECTED: Physical marksheet does not match University records! " +
                   std::to_string(report.tampered_fields_count) + " subject(s) modified.";
        }
        report.summary_message = msg;
    }

    return report;
}

VerificationReport Verifier::verifyImage(const MerkleTree& universityTree, const std::string& imagePath) {
    OCRResult ocr = OCREngine::scanImage(imagePath);
    return verify(universityTree, ocr);
}

void VerificationReport::printReport(std::ostream& os) const {
    os << "\n======================================================================\n";
    os << "                   THIRD-PARTY MARKSHEET AUDIT REPORT                \n";
    os << "                 (For Interviewers & Scholarship Givers)              \n";
    os << "======================================================================\n";

    if (verdict == VerificationVerdict::AUTHENTIC) {
        os << " VERDICT: [  AUTHENTIC & VERIFIED  ]\n";
    } else if (verdict == VerificationVerdict::TAMPERED) {
        os << " VERDICT: [! TAMPER / FRAUD DETECTED !]\n";
    } else if (verdict == VerificationVerdict::FORGED_NOT_FOUND) {
        os << " VERDICT: [! COUNTERFEIT / UNREGISTERED RECORD !]\n";
    } else {
        os << " VERDICT: [? OCR PROCESSING ERROR ?]\n";
    }

    os << "----------------------------------------------------------------------\n";
    os << " Student Roll No : " << (roll_number.empty() ? "N/A" : roll_number) << "\n";
    os << " Official Name   : " << (official_student_name.empty() ? (student_name.empty() ? "N/A" : student_name) : official_student_name) << "\n";
    if (name_mismatch) {
        os << " Marksheet Name  : " << paper_student_name << " [MISMATCH!]\n";
    }
    os << " University Root : " << official_root_hash.substr(0, 32) << "...\n";
    os << " Official Leaf   : " << (official_leaf_hash.empty() ? "N/A" : official_leaf_hash.substr(0, 32) + "...") << "\n";
    os << " Paper OCR Leaf  : " << (ocr_leaf_hash.empty() ? "N/A" : ocr_leaf_hash.substr(0, 32) + "...") << "\n";
    os << " Merkle Proof    : " << (merkle_proof_passed ? "PASSED (Cryptographically Valid)" : "FAILED (Tampered)") << "\n";
    os << "----------------------------------------------------------------------\n";
    os << " SUBJECT-BY-SUBJECT AUDIT BREAKDOWN:\n";
    os << " " << std::left << std::setw(12) << "Code"
       << std::left << std::setw(22) << "Subject Name"
       << std::right << std::setw(12) << "Paper (OCR)"
       << std::right << std::setw(14) << "Univ Official"
       << std::right << std::setw(10) << "Status\n";
    os << " --------------------------------------------------------------------\n";

    for (const auto& a : field_audits) {
        os << " " << std::left << std::setw(12) << a.subject_code
           << std::left << std::setw(22) << a.subject_name
           << std::right << std::setw(12) << (a.physical_ocr_marks >= 0 ? std::to_string(a.physical_ocr_marks) : "N/A")
           << std::right << std::setw(14) << a.official_marks
           << std::right << std::setw(10) << (a.is_match ? "MATCH" : "TAMPERED") << "\n";
    }

    os << "----------------------------------------------------------------------\n";
    os << " SUMMARY: " << summary_message << "\n";
    os << "======================================================================\n\n";
}

void Verifier::logAuditToCSV(const std::string& filepath, const std::string& imagePath, const VerificationReport& report) {
    bool fileExists = false;
    {
        std::ifstream check(filepath);
        fileExists = check.good();
    }

    std::ofstream out(filepath, std::ios::app);
    if (!out.is_open()) return;

    if (!fileExists) {
        out << "Timestamp,ImageFile,RollNumber,StudentName,Verdict,MerkleProof,TamperedSubjectCount,OfficialRoot,Summary\n";
    }

    std::time_t now = std::time(nullptr);
    char timeBuffer[64];
    std::strftime(timeBuffer, sizeof(timeBuffer), "%Y-%m-%d %H:%M:%S", std::localtime(&now));

    std::string verdictStr = "UNKNOWN";
    if (report.verdict == VerificationVerdict::AUTHENTIC) verdictStr = "AUTHENTIC";
    else if (report.verdict == VerificationVerdict::TAMPERED) verdictStr = "TAMPERED";
    else if (report.verdict == VerificationVerdict::FORGED_NOT_FOUND) verdictStr = "FORGED_NOT_FOUND";
    else if (report.verdict == VerificationVerdict::OCR_READ_ERROR) verdictStr = "OCR_ERROR";

    out << "\"" << timeBuffer << "\","
        << "\"" << imagePath << "\","
        << "\"" << report.roll_number << "\","
        << "\"" << report.student_name << "\","
        << "\"" << verdictStr << "\","
        << "\"" << (report.merkle_proof_passed ? "PASSED" : "FAILED") << "\","
        << report.tampered_fields_count << ","
        << "\"" << report.official_root_hash.substr(0, 16) << "...\","
        << "\"" << report.summary_message << "\"\n";

    out.close();
}
