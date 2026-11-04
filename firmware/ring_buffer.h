// ring_buffer.h - Fila circular de capacidade fixa (FIFO) usada na resiliencia offline.
//
// Regra de negocio: quando a fila enche, a amostra MAIS ANTIGA e descartada para abrir
// espaco para a nova, pois em monitoramento cardiaco o estado recente do paciente vale
// mais que o historico antigo. Cada descarte e contabilizado em dropped().
//
// A memoria e alocada uma unica vez, em begin(): um array estatico desse tamanho nao cabe
// no segmento DRAM do ESP32 junto com as bibliotecas de Wi-Fi e TLS.
//
// O codigo nao depende do Arduino, o que permite testa-lo no computador
// (ver tools/test_ring_buffer.cpp).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <new>

template <typename T>
class RingBuffer {
 public:
  // Aloca a fila. Retorna false se nao houver memoria para a capacidade pedida.
  bool begin(size_t capacity) {
    items_ = new (std::nothrow) T[capacity];
    capacity_ = items_ ? capacity : 0;
    return items_ != nullptr;
  }

  // Insere no fim da fila. Retorna false se precisou descartar a amostra mais antiga.
  bool push(const T &item) {
    if (capacity_ == 0) {
      dropped_++;
      return false;
    }
    bool keptAll = true;
    if (count_ == capacity_) {
      head_ = (head_ + 1) % capacity_;
      count_--;
      dropped_++;
      keptAll = false;
    }
    items_[(head_ + count_) % capacity_] = item;
    count_++;
    return keptAll;
  }

  // Le a amostra mais antiga SEM remove-la: ela so sai da fila com pop(),
  // chamado apos a confirmacao do envio. Assim, uma falha de rede nao perde dados.
  bool peek(T &out) const {
    if (count_ == 0) return false;
    out = items_[head_];
    return true;
  }

  // Remove a amostra mais antiga.
  bool pop() {
    if (count_ == 0) return false;
    head_ = (head_ + 1) % capacity_;
    count_--;
    return true;
  }

  size_t size() const { return count_; }
  size_t capacity() const { return capacity_; }
  bool empty() const { return count_ == 0; }
  uint32_t dropped() const { return dropped_; }

 private:
  T *items_ = nullptr;
  size_t capacity_ = 0;
  size_t head_ = 0;
  size_t count_ = 0;
  uint32_t dropped_ = 0;
};
