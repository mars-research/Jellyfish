#ifndef __REVERSIBLE_HASH__
#define __REVERSIBLE_HASH__

#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <stdint.h>
#include <iostream>
#include <utility>
#include <exception>
#include <assert.h>
#include "lib/misc.hpp"
#include "lib/reversible_hash_function.hpp"
#include "storage.hpp"
#include "offsets_key_value.hpp"

namespace jellyfish {
  namespace reversible_hash
    class InvalidMap : public std::exception {
      virtual const char* what() const throw() {
        return "Mapped region is invalid";
      }
    };

    /* (key,value) pair bit-packed array.  It implements the logic of the
     * packed hash except for size doubling. Setting or incrementing a key
     * will return false if the hash is full. No memory management is done
     * in this class either. A zero key is not valid.
     */
    template <typename word, typename atomic_t, typename mem_block_t>
    class array {
      typedef typename Offsets<word>::offset_t offset_t;
      static const word  wone = 1;
      size_t             size, size_mask;
      uint_t             reprobe_limit;
      uint_t             key_len;  // original key len
      Offsets<word>      offsets;  // key len used is reduced by size of hash array
      word               key_mask; // mask of bits for high bits of hash(key) stored in key field
      uint_t             key_off;  // offset in key field for reprobe value
      mem_block_t        mem_block;
      word              *data;
      word               zero_count;
      atomic_t           atomic;
      size_t            *reprobes;
      SquareBinaryMatrix hash_matrix;
      SquareBinaryMatrix hash_inverse_matrix;
      struct header {
        uint64_t size;
        uint64_t klen;
        uint64_t clen;
        uint64_t reprobe_limit;
      };

    public:
      array(size_t _size, uint_t _key_len, uint_t _val_len,
            uint_t _reprobe_limit, size_t *_reprobes) :
        size(((size_t)1) << ceilLog2(_size)), size_mask(size - 1), reprobe_limit(_reprobe_limit), 
        key_len(_key_len), offsets(ceilLog2(_size) - _key_len, _val_len, _reprobe_limit),
        key_mask(offsets.mask(offsets.get_key_len(), 0)), key_off(key_mask + 1),
        mem_block(div_ceil(size, (size_t)offsets.get_block_len()) * offsets.get_block_word_len() * sizeof(word)),
        data((word *)mem_block.get_ptr()), zero_count(0), reprobes(_reprobes),
        hash_matrix(key_len), hash_inverse_matrix(hash_matrix.init_random_inverse())
      {
        if(!data) {
          // TODO: should throw an error
          std::cerr << "allocation failed";
        }
      }
      
      array(char *map, size_t length) {
        if(length < sizeof(struct header))
          throw new InvalidMap();
        struct header *header = (struct header *)map;
        // TODO: Should make more consistency check on the map...
        size = header->size;
        size_mask = size - 1;
        offsets.init(header->klen, header->clen, header->reprobe_limit);
        map += sizeof(struct header);
        reprobes = new size_t[header->reprobe_limit];
        memcpy(reprobes, map, sizeof(size_t) * header->reprobe_limit);
        map += sizeof(size_t) * header->reprobe_limit;
        if((size_t)map & 0xf)
          map += 0x10 - ((size_t)map & 0xf); // Make sure aligned for 64bits word.
        zero_count = *(uint64_t *)map;
        map += sizeof(uint64_t);
        data = (word *)map;
      }

      ~array() {
        
      }

      size_t get_size() { return size; }
      uint_t get_key_len() { return key_len; }
      uint_t get_val_len() { return offsets.get_val_len(); }
      

      // Iterator
      class iterator {
        array  *ary;
        size_t  nid, end_id;
      public:
        word    key;
        word    val;
        size_t  id;
        
        iterator(array *_ary, size_t start, size_t end) :
          ary(_ary), nid(start), end_id(end) {}
        
        bool next() {
          while((id = nid++) < end_id) {
            if(ary->get_key_val_full(id, key, val)) {
              key = hash_inverse_matrix.times(key << | id);
              return true;
            }
          }
          return false;
        }
      };
      friend class iterator;
      iterator iterator_all() { return iterator(this, 0, get_size()); }
      iterator iterator_slice(size_t slice_number, size_t number_of_slice) {
        size_t slice_size = get_size() / number_of_slice;
        return iterator(this, slice_number * slice_size, (slice_number + 1) * slice_size);
      }

      /*
       * Return the key and value at position id. If the slot at id is
       * empty, returns false. If the slot at position id has the large
       * bit set, the key is resolved by looking backward in the
       * table. The value returned on the other hand is the value at
       * position id. No summation of the other entries for the key is
       * done.
       */
      bool get_key_val(size_t id, word &key, word &val) {
        word     *w, *kvw, *fw = NULL;
        offset_t *o, *lo, *fo  = NULL;
        bool      large;
        uint_t    overflows;

        overflows = 0;
        while(true) {
          w = offsets.get_word_offset(id, &o, &lo, data);
          kvw = w + o->key.woff;
          key = *kvw;
          large = key & o->key.lb_mask;
          if(large)
            o = lo;
          if(o->key.mask2) {
            key = (key & o->key.mask1 & ~o->key.sb_mask1) >> o->key.boff;
            key |= ((*(kvw+1)) & o->key.mask2 & ~o->key.sb_mask2) << o->key.shift;
          } else {
            key = (key & o->key.mask1) >> o->key.boff;
          }

          // Save offset and word for value retrieval
          if(!fo) {
            fo = o;
            fw = w;
          }

          if(large) {
            if(key)
              id -= reprobes[key];
            id = (id - reprobes[0]) & size_mask;
            overflows++;
          } else {
            break;
          }
        }
        if(!key)
          return false;

        kvw = fw + fo->val.woff;
        val = ((*kvw) & fo->val.mask1) >> fo->val.boff;
        if(fo->val.mask2) {
          val |= ((*(kvw+1)) & fo->val.mask2) << fo->val.shift;
        }
        if(overflows > 0) {
          val <<= offsets.get_val_len();
          if(--overflows > 0)
            val <<= offsets.get_lval_len() * overflows;
        }
        return true;
      }

      /*
       * Return the (high bits) of a key and value at position id. If
       * the slot at id is empty or has the large bit set, returns
       * false. Otherwise, returns the key and the value is the sum of
       * all the entries in the hash table for that key. I.e., the
       * table is search forward for entries with large bit set
       * pointing back to the key at id, and all those values are
       * summed up.
       */
      bool get_key_val_full(size_t id, word &key, word &val) {
        word     *w, *kvw, nkey, nval;
        offset_t *o, *lo;
        uint_t    reprobe = 0, overflows = 0;
        size_t    cid;

        w   = offsets.get_word_offset(id, &o, &lo, data);
        kvw = w + o->key.woff;
        key = *kvw;
        if(key & o->key.lb_mask)
          return false;
        if(o->key.mask2) {
          if((key & o->key.sb_mask1) == 0)
            return false;
          key  = (key & o->key.mask1 & ~o->key.sb_mask1) >> o->key.boff;
          key |= ((*(kvw+1)) & o->key.mask2 & ~o->key.sb_mask2) << o->key.shift; 
        } else {
          key = (key & o->key.mask1) >> o->key.boff;
          if(key == 0)
            return false;
        }

        key &= key_mask;
        kvw = w + o->val.woff;
        val = ((*kvw) & o->val.mask1) >> o->val.boff;
        if(o->val.mask2)
          val |= ((*(kvw+1)) & o->val.mask2) << o->val.shift;

        // Resolve value
        reprobe = 0;
        cid = id = (id + reprobes[0]) & size_mask;
        while(reprobe <= reprobe_limit) {
          if(reprobe)
            cid  = (id + reprobes[reprobe]) & size_mask;

          w    = offsets.get_word_offset(cid, &o, &lo, data);
          kvw  = w + o->key.woff;
          nkey = *kvw;
          if(nkey & o->key.lb_mask) {
            if(lo->key.mask2) {
              nkey  = (nkey & lo->key.mask1 & ~lo->key.sb_mask1) >> lo->key.boff;
              nkey |= ((*(kvw+1)) & lo->key.mask2 & ~lo->key.sb_mask2) << lo->key.shift;
            } else {
              nkey = (nkey & lo->key.mask1) >> lo->key.boff;
            }
            if(nkey == reprobe) {
              kvw = w + lo->val.woff;
              nval = ((*kvw) & lo->val.mask1) >> lo->val.boff;
              if(lo->val.mask2)
                nval |= ((*(kvw+1)) & lo->val.mask2) << lo->val.shift;
              nval <<= offsets.get_val_len();
              if(overflows > 0)
                nval <<= offsets.get_lval_len() * overflows;
              val += nval;
          
              //           overflows++;
              //           reprobe = 0;
              //           id = (cid + reprobes[0]) & size_mask;
              return true;
            }
          } else {
            if(o->key.mask2) {
              if((nkey & o->key.sb_mask1) == 0)
                return true;
            } else {
              if((nkey & o->key.mask1) == 0)
                return true;
            }
          }

          reprobe++;
        }

        return true;
      }

      bool get_val(size_t id, word key, word &val, bool full = false) {
        word     *w, *kvw, nkey, nval;
        offset_t *o, *lo;
        uint_t    reprobe = 0;
        size_t    cid = id;
    
        // Find key
        while(true) {
          w   = offsets.get_word_offset(cid, &o, &lo, data);
          kvw = w + o->key.woff;
          nkey = *kvw;
      
          if(!(nkey & o->key.lb_mask)) {
            if(o->key.mask2) {
              nkey = (nkey & o->key.mask1 & ~o->key.sb_mask1) >> o->key.boff;
              nkey |=  ((*(kvw+1)) & o->key.mask2 & ~o->key.sb_mask2) << o->key.shift;
            } else {
              nkey = (nkey & o->key.mask1) >> o->key.boff;
            }
            if(nkey == key)
              break;
          }
          if(++reprobe > reprobe_limit)
            return false;
          cid = (id + reprobes[reprobe]) & size_mask;
        }

        // Get value
        kvw = w + o->val.woff;
        val = ((*kvw) & o->val.mask1) >> o->val.boff;
        if(o->val.mask2) {
          val |= ((*(kvw+1)) & o->val.mask2) << o->val.shift;
        }

        // Eventually get large values...
        if(full) {
          cid = id = (cid + reprobes[0]) & size_mask;
          reprobe = 0;
          do {
            w   = offsets.get_word_offset(cid, &o, &lo, data);
            kvw = w + o->key.woff;
            nkey = *kvw;
            if(nkey & o->key.lb_mask) {
              if(lo->key.mask2) {
                nkey = (nkey & lo->key.mask1 & ~lo->key.sb_mask1) >> lo->key.boff;
                nkey |=  ((*(kvw+1)) & lo->key.mask2 & ~lo->key.sb_mask2) << lo->key.shift;
              } else {
                nkey = (nkey & lo->key.mask1) >> lo->key.boff;
              }
              if(nkey == reprobe) {
                kvw = w + lo->val.woff;
                nval = ((*kvw) & lo->val.mask1) >> lo->val.boff;
                if(lo->val.mask2)
                  nval |= ((*(kvw+1)) & lo->val.mask2) << lo->val.shift;

                val |= nval << offsets.get_val_len();
                break;
              }
            }

            cid = (id + reprobes[++reprobe]) & size_mask;
          } while(reprobe <= reprobe_limit);
        }

        return true;
      }
  
      inline bool add(word key, word val) {
        uint64_t hash = hash_matrix.times(key);
        return add_rec(hash & size_mask, hash & high_bits_mask, val, false);
      }

      bool add_rec(size_t id, word key, word val, bool large) {
        uint_t    reprobe     = 0;
        offset_t *o, *lo, *ao;
        word     *w, *kw, *vw, nkey;
        bool      key_claimed = false;
        size_t    cid         = id;
        word      cary;
        word      akey        = large ? 0 : key | (1 << key_off);

        // Claim key
        do {
          w = offsets.get_word_offset(cid, &o, &lo, data);
          ao = large ? lo : o;

          kw = w + ao->key.woff;

          if(ao->key.mask2) { // key split on two words
            nkey = akey << ao->key.boff;
            nkey |= ao->key.sb_mask1;
            if(large)
              nkey |= ao->key.lb_mask;
            nkey &= ao->key.mask1;

            // Use o->key.mask1 and not ao->key.mask1 as the first one is
            // guaranteed to be bigger. The key needs to be free on its
            // longer mask to claim it!
            key_claimed = set_key(kw, nkey, o->key.mask1, ao->key.mask1);
            if(key_claimed) {
              nkey = ((akey >> ao->key.shift) | ao->key.sb_mask2) & ao->key.mask2;
              key_claimed = key_claimed && set_key(kw + 1, nkey, o->key.mask2, ao->key.mask2);
            }
          } else { // key on one word
            nkey = akey << ao->key.boff;
            if(large)
              nkey |= ao->key.lb_mask;
            nkey &= ao->key.mask1;
            key_claimed = set_key(kw, nkey, o->key.mask1, ao->key.mask1);
          }
          if(!key_claimed) { // reprobe
            if(++reprobe > reprobe_limit)
              return false;
            cid = (id + reprobes[reprobe]) & size_mask;

            if(large)
              akey = reprobe;
            else
              akey = (akey & key_mask) | ((reprobe + 1) << key_off);
          }
        } while(!key_claimed);

        // Increment value
        vw = w + ao->val.woff;
        cary = add_val(vw, val, ao->val.boff, ao->val.mask1);
        cary >>= ao->val.shift;
        if(cary && ao->val.mask2) { // value split on two words
          cary = add_val(vw + 1, cary, 0, ao->val.mask2);
          cary >>= ao->val.cshift;
        }
        if(cary) {
          cid = (cid + reprobes[0]) & size_mask;
          if(add_rec(cid, key, cary, true))
            return true;

          // Adding failed, table is full. Need to back-track and
          // substract val.
          cary = add_val(vw, (1 << offsets.get_val_len()) - val,
                         ao->val.boff, ao->val.mask1);
          cary >>= ao->val.shift;
          if(cary && ao->val.mask2) {
            // Can I ignore the cary here? Table is known to be full, so
            // not much of a choice. But does it leave the table in a
            // consistent state?
            add_val(vw + 1, cary, 0, ao->val.mask2);
          }
          return false;
        }
        return true;
      }

      void write_raw(std::ostream &out) {
        struct header header = { size, offsets.get_key_len(), offsets.get_val_len(), reprobe_limit };
        out.write((char *)&header, sizeof(header));
        out.write((char *)reprobes, sizeof(size_t) * reprobe_limit);
        
        if(out.tellp() & 0xf) { // Make sure aligned
          string padding(0x10 - (out.tellp() & 0xf), '\0');
          out.write(padding.c_str(), padding.size());
        }
        out.write((char *)&zero_count, sizeof(word));
        out.write((char *)mem_block.get_ptr(), mem_block.get_size());
      }

    private:
      inline bool set_key(word *w, word nkey, word free_mask, 
                          word equal_mask) {
        word ow = *w, nw, okey;

        okey = ow & free_mask;
        while(okey == 0) { // large bit not set && key is free
          nw = atomic.cas(w, ow, ow | nkey);
          if(nw == ow)
            return true;
          ow = nw;
          okey = ow & free_mask;
        }
        return (ow & equal_mask) == nkey;
      }

      inline word add_val(word *w, word val, uint_t shift, word mask) {
        word now = *w, ow, nw, nval;

        do {
          ow = now;
          nval = ((ow & mask) >> shift) + val;
          nw = (ow & ~mask) | ((nval << shift) & mask);
          now = atomic.cas(w, ow, nw);
        } while(now != ow);

        return nval & (~(mask >> shift));
      }
    };

    /*****/
  }
}

#endif // __REVERSIBLE_HASH__