#pragma once

#include <vector>
#include <cstdint>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <cstring>

namespace ClimaxEngine {
namespace RWS {

enum RwStreamType {
    rwNASTREAM = 0,
    rwSTREAMFILE,
    rwSTREAMFILENAME,
    rwSTREAMMEMORY,
    rwSTREAMCUSTOM,
};

enum RwStreamAccessType {
    rwNASTREAMACCESS = 0,
    rwSTREAMREAD,
    rwSTREAMWRITE,
    rwSTREAMAPPEND,
};

struct RwMemory {
    uint8_t* start;
    uint32_t length;
};

class RwStream {
public:
    virtual ~RwStream() = default;

    virtual size_t Read(void* buffer, size_t length) = 0;
    virtual size_t Write(const void* buffer, size_t length) = 0;
    virtual size_t Skip(size_t length) = 0;
    virtual size_t Tell() const = 0;
    virtual void Seek(size_t pos) = 0;
    virtual bool IsEOF() const = 0;
};

class RwMemoryStream : public RwStream {
public:
    RwMemoryStream(const std::vector<uint8_t>& data) 
        : m_data(data), m_pos(0) {}
        
    RwMemoryStream(const uint8_t* data, size_t size)
        : m_data(data, data + size), m_pos(0) {}

    size_t Read(void* buffer, size_t length) override {
        if (m_pos + length > m_data.size()) {
            length = m_data.size() - m_pos;
        }
        if (length > 0) {
            std::memcpy(buffer, m_data.data() + m_pos, length);
            m_pos += length;
        }
        return length;
    }

    size_t Write(const void* buffer, size_t length) override {
        throw std::runtime_error("RwMemoryStream is read-only");
    }

    size_t Skip(size_t length) override {
        if (m_pos + length > m_data.size()) {
            length = m_data.size() - m_pos;
        }
        m_pos += length;
        return length;
    }

    size_t Tell() const override {
        return m_pos;
    }
    
    void Seek(size_t pos) override {
        if (pos > m_data.size()) {
            m_pos = m_data.size();
        } else {
            m_pos = pos;
        }
    }
    
    bool IsEOF() const override {
        return m_pos >= m_data.size();
    }

    // Custom helper for chunk parsing
    const uint8_t* GetCurrentPointer() const {
        return m_data.data() + m_pos;
    }
    
    size_t GetRemainingSize() const {
        return m_data.size() - m_pos;
    }

private:
    std::vector<uint8_t> m_data;
    size_t m_pos;
};

} // namespace RWS
} // namespace ClimaxEngine
