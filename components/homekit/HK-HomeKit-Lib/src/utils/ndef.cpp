/*
  Code highly inspired by https://github.com/kormax/apple-home-key-reader/blob/main/util/ndef.py
 */

#include "ndef.h"
#include <string.h>
#include "logging.h"

NDEFRecord::NDEFRecord() {
  this->id.assign(1, '\0');
  this->type.assign(1, '\0');
  this->data.assign(1, '\0');
  this->tnf = 0x00;
}

NDEFRecord::NDEFRecord(const char *id, unsigned char tnf, const char *type, unsigned char *data, size_t dataLen)
{
  this->id.insert(this->id.begin(), id, id + strlen(id));
  this->id.push_back('\0');
  this->type.insert(this->type.begin(), type, type + strlen(type));
  this->type.push_back('\0');
  this->data.insert(this->data.begin(), data, data + dataLen);
  this->data.push_back('\0');
  this->tnf = tnf;
}

NDEFRecord::NDEFRecord(std::vector<unsigned char> id, unsigned char tnf, std::vector<unsigned char> type, std::vector<unsigned char> data){
  this->id = id;
  this->type = type;
  this->data = data;
  this->tnf = tnf;
}

NDEFMessage::NDEFMessage(unsigned char *data, size_t length){
  this->packedData.insert(this->packedData.begin(), data, data + length);
}

NDEFMessage::NDEFMessage(std::initializer_list<NDEFRecord> records)
{
  this->records.assign(records);
}

std::vector<unsigned char> NDEFMessage::pack()
{
  unsigned char result[256];
  size_t olen = 0;
  for (size_t i = 0; i < this->records.size(); i++)
  {
    unsigned char id_length = this->records.data()[i].id.size() - 1;
    unsigned char type_length = this->records.data()[i].type.size() - 1;
    unsigned char payload_length = this->records.data()[i].data.size() - 1;
    unsigned char mb = (1 << 8) >> 1 * (i == 0);
    unsigned char me = (1 << 8) >> 2 * (i == this->records.size() - 1);
    unsigned char ch = 0x0;
    unsigned char sr = (1 << 8) >> 4 * (payload_length <= 255);
    unsigned char il = (1 << 8) >> 5 * (id_length > 0);
    unsigned char tnf = this->records.data()[i].tnf;
    unsigned char header = mb + me + ch + sr + il + tnf;
    unsigned char packed[sizeof(header) + 1 + 1 + (id_length > 0 ? 1 : 0) + type_length + id_length + payload_length] = {
        header,
        (unsigned char)type_length,
        (unsigned char)payload_length,
        id_length,
    };
    memcpy(packed + sizeof(header) + 1 + 1 + (id_length > 0 ? 1 : 0), this->records.data()[i].type.data(), type_length);
    memcpy(packed + sizeof(header) + 1 + 1 + (id_length > 0 ? 1 : 0) + type_length, this->records.data()[i].id.data(), id_length);
    memcpy(packed + sizeof(header) + 1 + 1 + (id_length > 0 ? 1 : 0) + type_length + id_length, this->records.data()[i].data.data(), payload_length);
    memcpy(result + olen, packed, sizeof(packed));
    olen += sizeof(packed);
  }
  this->packedData.clear();
  this->packedData.insert(this->packedData.begin(), result, result + olen);
  LOG(D, "NDEF MSG PACKED - LENGTH: %d, DATA: %s", packedData.size(), red_log::bufToHexString(packedData.data(), packedData.size()).c_str());
  return this->packedData;
}

std::vector<NDEFRecord> NDEFMessage::unpack(){
  // HAP-ESPHome patch: every field is bounds-checked against the received
  // message (it comes from the NFC peer); a truncated record ends parsing.
  std::vector<NDEFRecord> records;
  const size_t size = this->packedData.size();
  const unsigned char *buf = this->packedData.data();
  size_t i = 0;
  while(i < size)
  {
    unsigned char header = buf[i++];
    unsigned char sr = ((header >> 4 << 7) & 0xFF) >> 3;
    unsigned char il = ((header >> 3 << 7) & 0xFF) >> 4;
    unsigned char tnf = ((header << 5) & 0xFF) >> 5;

    if (i >= size) break;
    size_t type_length = buf[i++];

    size_t payload_length = 0;
    if(sr){
      if (i >= size) break;
      payload_length = buf[i++];
    }
    else
    {
      if (size - i < 4) break;
      payload_length = (size_t(buf[i]) << 24) | (size_t(buf[i + 1]) << 16) | (size_t(buf[i + 2]) << 8) | buf[i + 3];
      i += 4;
    }

    size_t id_length = 0;
    if(il){
      if (i >= size) break;
      id_length = buf[i++];
    }

    if (type_length > size - i) break;
    std::vector<unsigned char> type_vec(buf + i, buf + i + type_length);
    type_vec.push_back('\0');
    i += type_length;

    if (id_length > size - i) break;
    std::vector<unsigned char> id_vec;
    id_vec.resize(id_length + 1);
    id_vec.insert(id_vec.begin(), buf + i, buf + i + id_length);
    id_vec.push_back('\0');
    i += id_length;

    if (payload_length > size - i) break;
    std::vector<unsigned char> payload_vec(buf + i, buf + i + payload_length);
    payload_vec.push_back('\0');
    i += payload_length;

    LOG(D, "NDEF RECORD ID: %s, TNF: %d, TYPE: %s, PAYLOAD: %s", red_log::bufToHexString(id_vec.data(), id_vec.size()).c_str(), (int)tnf, red_log::bufToHexString(type_vec.data(), type_vec.size()).c_str(), red_log::bufToHexString(payload_vec.data(), payload_vec.size()).c_str());
    records.emplace_back(id_vec, tnf, type_vec, payload_vec);
  }
  this->records.insert(this->records.begin(), records.data(), records.data() + records.size());
  return this->records;
}

NDEFRecord* NDEFMessage::findType(const char * type){
  NDEFRecord *foundRecord = nullptr;
  for (auto &&record : records)
  {
    if(!strcmp(type, (const char *)record.type.data())){
      foundRecord = &record;
      break;
    }
  }
  if (foundRecord == nullptr) {  // HAP-ESPHome patch: no null dereference
    LOG(D, "NDEF RECORD of type %s not found", type);
    return nullptr;
  }
  LOG(D, "NDEF RECORD ID: %s, TNF: %s, TYPE: %s, PAYLOAD: %s", red_log::bufToHexString(foundRecord->id.data(), foundRecord->id.size()).c_str(), red_log::bufToHexString(&foundRecord->tnf, 1).c_str(), red_log::bufToHexString(foundRecord->type.data(), foundRecord->type.size()).c_str(), red_log::bufToHexString(foundRecord->data.data(), foundRecord->data.size()).c_str());
  return foundRecord;
}