#pragma once

#include <QString>

namespace usa::scanner {
struct EngineResult;
}

class ScanLogger {
public:
    static void writeLog(const QString& taskId, const QString& targetPath, const usa::scanner::EngineResult& result);
};
