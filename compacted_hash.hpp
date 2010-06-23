#ifndef __COMPACTED_HASH__
#define __COMPACTED_HASH__

#include <iostream>
#include <fstream>
#include <string.h>
#include <pthread.h>
#include "storage.hpp"

namespace jellyfish {
  namespace compacted_hash {
    struct header {
      uint64_t  mer_len;
      uint64_t  val_len; // In bytes
      uint64_t  size; // In bytes
      uint64_t  unique;
      uint64_t  distinct;
      uint64_t  total;
    };
    class ErrorReading : public std::exception {
      std::string msg;
    public:
      ErrorReading(const std::string _msg) : msg(_msg) { }
      virtual ~ErrorReading() throw() {}
      virtual const char* what() const throw() {
        return msg.c_str();
      }
    };
    
    template <typename hash_iterator_t, typename word>
    class writer
    {
      uint_t    mer_len, key_len, val_len;
      size_t    record_len, nb_record, buffer_len;
      word      max_count;
      char     *buffer, *end_buffer;
      uint64_t  unique, distinct, total;

    public:
      // key_len and val_len are given in bytes.
      // buffer_size is in bytes.
      writer(size_t _buffer_size, uint_t _mer_len, uint_t _val_len) :
        mer_len(_mer_len), 
        key_len(_mer_len / 4 + (_mer_len % 4 != 0)),
        val_len(_val_len), record_len(key_len + val_len), 
        nb_record(_buffer_size / record_len),
        buffer_len(nb_record * record_len), max_count((((word)1) << (8*val_len)) - 1),
        unique(0), distinct(0), total(0)
      {
        buffer = new char[buffer_len];
        end_buffer = buffer + buffer_len;
      }

      ~writer() { delete buffer; }
      
      bool write_header(std::ostream *out, size_t size);
      bool dump(std::ostream *out, hash_iterator_t &it, pthread_mutex_t *lock);
      bool update_stats(std::ostream *out, uint64_t unique,
                      uint64_t distinct, uint64_t total);


      uint_t get_mer_len() { return mer_len; }
      uint_t get_val_len() { return val_len; }
      uint64_t get_unique() { return unique; }
      uint64_t get_distinct() { return distinct; }
      uint64_t get_total() { return total; }
    };
      
    class reader {
      struct header header;
      std::ifstream io;
      uint_t	 key_len, val_len;
      size_t	 record_len, nb_record, buffer_len;
      uint64_t		 max_count;
      char		*buffer, *end_buffer;

    public:
      reader(std::string filename) {
        io.open(filename.c_str(), std::ios::binary);
        if(!io.good())
          throw new ErrorReading("Can't open file");
        io.read((char *)&header, sizeof(header));
        if(!io.good())
          throw new ErrorReading("Error reading header");
        key_len  = (header.mer_len / 4) + (header.mer_len % 4 != 0);
      }

      uint_t get_mer_len() { return header.mer_len; }
      uint_t get_val_len() { return header.val_len; }
      uint64_t get_unique() { return header.unique; }
      uint64_t get_distinct() { return header.distinct; }
      uint64_t get_total() { return header.total; }
    };

    template <typename hash_iterator_t, typename word>
    bool writer<hash_iterator_t,word>::write_header(std::ostream *out,
                                                                 size_t size)
    {
      struct header header;
    
      memset(&header, '\0', sizeof(header));
      header.mer_len = mer_len;
      header.val_len = val_len;
      header.size = size;
      out->write((char *)&header, sizeof(header));
      return !out->bad();
    }
  
    template <typename hash_iterator_t, typename word>
    bool writer<hash_iterator_t,word>::update_stats(std::ostream *out,
                                                                 uint64_t unique, uint64_t distinct, uint64_t total)
    {
      struct header header;
      std::streampos pos = (char *)&header.unique - (char *)&header;
      header.unique = unique;
      header.distinct = distinct;
      header.total = total;
      out->seekp(pos);
      out->write((char *)&header.unique, sizeof(header) - pos);
      return !out->bad();
    }
  

    template <typename hash_iterator_t, typename word>
    bool writer<hash_iterator_t,word>::dump(std::ostream *out,
                                                         hash_iterator_t &it,
                                                         pthread_mutex_t *lock)
    {
      char *ptr;
      word count;

      while(true) {
        ptr = buffer;
        while(ptr < end_buffer) {
          if(!it.next())
            break;
          memcpy(ptr, &it.key, key_len);
          ptr += key_len;
          count = (it.val > max_count) ? max_count : it.val;
          memcpy(ptr, &count, val_len);
          ptr += val_len;
          if(it.val == 1)
            unique++;
          distinct++;
          total += it.val;
        }
        if(ptr == buffer)
          break;

        pthread_mutex_lock(lock);
        out->write(buffer, ptr - buffer);
        pthread_mutex_unlock(lock);
        if(out->bad())
          return false;
      }

      return true;
    }
  }
}
#endif /* __COMPACTED_HASH__ */