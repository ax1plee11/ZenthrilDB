#pragma once

#include "core/Types.hpp"

#include <span>
#include <vector>

namespace zenthrildb {

class IEncryptionProvider {
public:
  virtual ~IEncryptionProvider() = default;
  virtual std::vector<Byte> encrypt(std::span<const Byte> plaintext) = 0;
  virtual std::vector<Byte> decrypt(std::span<const Byte> ciphertext) = 0;
};

class IKeyManager {
public:
  virtual ~IKeyManager() = default;
  virtual void setKey(std::span<const Byte> keyMaterial) = 0;
  [[nodiscard]] virtual std::vector<Byte> getKey() const = 0;
};

} // namespace zenthrildb
