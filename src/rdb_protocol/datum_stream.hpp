// Copyright 2010-2013 RethinkDB, all rights reserved.
#ifndef RDB_PROTOCOL_DATUM_STREAM_HPP_
#define RDB_PROTOCOL_DATUM_STREAM_HPP_

#include <algorithm>
#include <deque>
#include <list>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "errors.hpp"
#include <boost/enable_shared_from_this.hpp>
#include <boost/function.hpp>
#include <boost/scoped_ptr.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/variant/get.hpp>

#include "clustering/administration/namespace_interface_repository.hpp"
#include "rdb_protocol/protocol.hpp"

namespace ql {

class scope_env_t;

enum batch_type_t {
    NORMAL = 0,
    // We sometimes need a batch with constant sindex for sorting.
    SINDEX_CONSTANT = 1
};

class datum_stream_t : public single_threaded_countable_t<datum_stream_t>,
                       public pb_rcheckable_t {
public:
    virtual ~datum_stream_t() { }

    // stream -> stream
    virtual counted_t<datum_stream_t> filter(counted_t<func_t> f,
                                             counted_t<func_t> default_filter_val) = 0;
    virtual counted_t<datum_stream_t> map(counted_t<func_t> f) = 0;
    virtual counted_t<datum_stream_t> concatmap(counted_t<func_t> f) = 0;

    // stream -> atom
    virtual counted_t<const datum_t> count(env_t *env) = 0;
    virtual counted_t<const datum_t> reduce(env_t *env,
                                            counted_t<val_t> base_val,
                                            counted_t<func_t> f) = 0;
    virtual counted_t<const datum_t> gmr(env_t *env,
                                         counted_t<func_t> g,
                                         counted_t<func_t> m,
                                         counted_t<const datum_t> d,
                                         counted_t<func_t> r) = 0;


    // stream -> stream (always eager)
    counted_t<datum_stream_t> slice(size_t l, size_t r);
    counted_t<datum_stream_t> zip();
    counted_t<datum_stream_t> indexes_of(counted_t<func_t> f);

    // Returns false or NULL respectively if stream is lazy.
    virtual bool is_array() = 0;
    virtual counted_t<const datum_t> as_array(env_t *env) = 0;

    // Gets the next elements from the stream.  (Returns zero elements only when
    // the end of the stream has been reached.  Otherwise, returns at least one
    // element.)  (Wrapper around `next_batch_impl`.)
    std::vector<counted_t<const datum_t> >
    next_batch(env_t *env, batch_type_t batch_type = NORMAL);

protected:
    explicit datum_stream_t(const protob_t<const Backtrace> &bt_src)
        : pb_rcheckable_t(bt_src) { }

private:
    static const size_t MAX_BATCH_SIZE = 100;

    virtual std::vector<counted_t<const datum_t> >
    next_batch_impl(env_t *env, batch_type_t batch_type) = 0;
};

class eager_datum_stream_t : public datum_stream_t {
protected:
    explicit eager_datum_stream_t(const protob_t<const Backtrace> &bt_src)
        : datum_stream_t(bt_src) { }

    virtual counted_t<datum_stream_t> filter(counted_t<func_t> f,
                                             counted_t<func_t> default_filter_val);
    virtual counted_t<datum_stream_t> map(counted_t<func_t> f);
    virtual counted_t<datum_stream_t> concatmap(counted_t<func_t> f);

    virtual counted_t<const datum_t> count(env_t *env);
    virtual counted_t<const datum_t> reduce(env_t *env,
                                            counted_t<val_t> base_val,
                                            counted_t<func_t> f);
    virtual counted_t<const datum_t> gmr(env_t *env,
                                         counted_t<func_t> g,
                                         counted_t<func_t> m,
                                         counted_t<const datum_t> d,
                                         counted_t<func_t> r);

    virtual bool is_array() { return true; }
    virtual counted_t<const datum_t> as_array(env_t *env);
private:
    std::vector<counted_t<const datum_t> >
    virtual next_batch_impl(env_t *env, batch_type_t batch_type);
};

class wrapper_datum_stream_t : public eager_datum_stream_t {
public:
    explicit wrapper_datum_stream_t(counted_t<datum_stream_t> _source)
        : eager_datum_stream_t(_source->backtrace()), source(_source) { }
    virtual bool is_array() { return source->is_array(); }
    virtual counted_t<const datum_t> as_array(env_t *env) {
        return is_array()
            ? eager_datum_stream_t::as_array(env)
            : counted_t<const datum_t>();
    }

protected:
    const counted_t<datum_stream_t> source;
};

class map_datum_stream_t : public wrapper_datum_stream_t {
public:
    map_datum_stream_t(counted_t<func_t> _f, counted_t<datum_stream_t> _source);

private:
    std::vector<counted_t<const datum_t> > next_batch_impl(env_t *env);

    counted_t<func_t> f;
};

class indexes_of_datum_stream_t : public wrapper_datum_stream_t {
public:
    indexes_of_datum_stream_t(counted_t<func_t> _f, counted_t<datum_stream_t> _source);

private:
    std::vector<counted_t<const datum_t> > next_batch_impl(env_t *env);

    counted_t<func_t> f;
    int64_t index;
};

class filter_datum_stream_t : public wrapper_datum_stream_t {
public:
    filter_datum_stream_t(counted_t<func_t> _f,
                          counted_t<func_t> _default_filter_val,
                          counted_t<datum_stream_t> _source);

private:
    std::vector<counted_t<const datum_t> > next_batch_impl(env_t *env);

    counted_t<func_t> f;
    counted_t<func_t> default_filter_val;
};

class concatmap_datum_stream_t : public wrapper_datum_stream_t {
public:
    concatmap_datum_stream_t(counted_t<func_t> _f, counted_t<datum_stream_t> _source);

private:
    std::vector<counted_t<const datum_t> > next_batch_impl(env_t *env);

    counted_t<func_t> f;
    std::vector<counted_t<datum_stream_t> > subsources;
    size_t index;
};

class datum_range_t {
public:
    datum_range_t(
        counted_t<const datum_t> left_bound, key_range_t::bound_t left_bound_type,
        counted_t<const datum_t> right_bound, key_range_t::bound_t right_bound_type);
    static datum_range_t universe();
private:
    friend class readgen_t;
    friend class primary_readgen_t;
    friend class secondary_readgen_t;
    key_range_t to_primary_keyrange() const;
    key_range_t to_secondary_keyrange() const;
    const counted_t<const datum_t> left_bound, right_bound;
    const key_range_t::bound_t left_bound_type, right_bound_type;
};

// RSI: remove?
typedef rdb_protocol_details::transform_t transform_t;
typedef rdb_protocol_details::terminal_t terminal_t;
typedef rdb_protocol_details::rget_item_t rget_item_t;
typedef rdb_protocol_details::transform_variant_t transform_variant_t;
typedef rdb_protocol_t::read_t read_t;
typedef rdb_protocol_t::rget_read_t rget_read_t;
typedef rdb_protocol_t::read_response_t read_response_t;
typedef rdb_protocol_t::rget_read_response_t rget_read_response_t;
typedef rdb_protocol_t::region_t region_t;

class readgen_t {
public:
    explicit readgen_t(
        const std::map<std::string, wire_func_t> &global_optargs,
        const datum_range_t &original_datum_range,
        sorting_t sorting);
    virtual ~readgen_t() { }
    rget_read_t terminal_read(const transform_t &transform, terminal_t &&_terminal);
    // This has to be on `readgen_t` because we sort differently depending on
    // the kinds of reads we're doing.
    virtual void sindex_sort(std::vector<rget_item_t> *vec) = 0;

    virtual rget_read_t next_read(
        const key_range_t &active_range, const transform_t &transform);
    virtual boost::optional<rget_read_t> sindex_sort_read(
        const std::vector<rget_item_t> &items) = 0;
    virtual key_range_t original_keyrange() = 0;

    // Returns `true` if there is no more to read.
    bool update_range(key_range_t *active_range,
                      const store_key_t &last_considered_key);
protected:
    const std::map<std::string, wire_func_t> global_optargs;
    const datum_range_t original_datum_range;
    const sorting_t sorting;

private:
    virtual rget_read_t next_read_impl(
        const key_range_t &active_range, const transform_t &transform) = 0;
};

class primary_readgen_t : public readgen_t {
public:
    primary_readgen_t(
        const std::map<std::string, wire_func_t> &global_optargs,
        datum_range_t range = datum_range_t::universe(),
        sorting_t sorting = UNORDERED);
private:
    virtual rget_read_t next_read_impl(
        const key_range_t &active_range, const transform_t &transform);
    virtual boost::optional<rget_read_t> sindex_sort_read(
        const std::vector<rget_item_t> &items);
    virtual void sindex_sort(std::vector<rget_item_t> *vec);
    virtual key_range_t original_keyrange();
};

class sindex_readgen_t : public readgen_t {
public:
    sindex_readgen_t(
        const std::map<std::string, wire_func_t> &global_optargs,
        const std::string &sindex,
        datum_range_t sindex_range = datum_range_t::universe(),
        sorting_t sorting = UNORDERED);
private:
    const std::string sindex;
    virtual rget_read_t next_read_impl(
        const key_range_t &active_range, const transform_t &transform);
    virtual boost::optional<rget_read_t> sindex_sort_read(
        const std::vector<rget_item_t> &items);
    virtual void sindex_sort(std::vector<rget_item_t> *vec);
    virtual key_range_t original_keyrange();
};

// RSI: prefetching
class reader_t {
public:
    explicit reader_t(
        const namespace_repo_t<rdb_protocol_t>::access_t &ns_access,
        bool use_outdated,
        scoped_ptr_t<readgen_t> &&readgen);
    void add_transformation(transform_variant_t &&tv);
    std::vector<counted_t<const datum_t> >
    next_batch(env_t *env, batch_type_t batch_type);
private:
    // Returns `true` if there's data in `items`.
    bool load_items(env_t *env);
    read_response_t do_read(env_t *env, const read_t &read);
    std::vector<rget_item_t> do_range_read(env_t *env, const read_t &read);

    namespace_repo_t<rdb_protocol_t>::access_t ns_access;
    const bool use_outdated;
    transform_t transform;

    bool started, finished;
    // RSI: make const?
    scoped_ptr_t<readgen_t> readgen;
    key_range_t active_range;

    // We need this to handle the SINDEX_CONSTANT case.
    std::vector<rget_item_t> items;
    size_t items_index;
};

class lazy_datum_stream_t : public datum_stream_t {
public:
    lazy_datum_stream_t(env_t *env, bool use_outdated,
                        namespace_repo_t<rdb_protocol_t>::access_t *ns_access,
                        const protob_t<const Backtrace> &bt_src);

    virtual counted_t<datum_stream_t> filter(counted_t<func_t> f,
                                             counted_t<func_t> default_filter_val);
    virtual counted_t<datum_stream_t> map(counted_t<func_t> f);
    virtual counted_t<datum_stream_t> concatmap(counted_t<func_t> f);

    virtual counted_t<const datum_t> count(env_t *env);
    virtual counted_t<const datum_t> reduce(env_t *env,
                                            counted_t<val_t> base_val,
                                            counted_t<func_t> f);
    virtual counted_t<const datum_t> gmr(env_t *env,
                                         counted_t<func_t> g,
                                         counted_t<func_t> m,
                                         counted_t<const datum_t> base,
                                         counted_t<func_t> r);
    virtual bool is_array() { return false; }
    virtual counted_t<const datum_t> as_array(UNUSED env_t *env) {
        return counted_t<const datum_t>();  // Cannot be converted implicitly.
    }

private:
    counted_t<const datum_t> next_impl(env_t *env);
    std::vector<counted_t<const datum_t> >
    next_batch_impl(env_t *env, batch_type_t batch_type);

    rdb_protocol_t::rget_read_response_t::result_t run_terminal(
        env_t *env, const rdb_protocol_details::terminal_variant_t &t);

    size_t current_batch_offset;
    std::vector<counted_t<const datum_t> > current_batch;

    reader_t reader;
};

class array_datum_stream_t : public eager_datum_stream_t {
public:
    array_datum_stream_t(counted_t<const datum_t> _arr,
                         const protob_t<const Backtrace> &bt_src);

private:
    counted_t<const datum_t> next_impl(env_t *env);

    size_t index;
    counted_t<const datum_t> arr;
};

class slice_datum_stream_t : public wrapper_datum_stream_t {
public:
    slice_datum_stream_t(size_t left, size_t right, counted_t<datum_stream_t> src);
private:
    counted_t<const datum_t> next_impl(env_t *env);
    uint64_t index, left, right;
};

class zip_datum_stream_t : public wrapper_datum_stream_t {
public:
    explicit zip_datum_stream_t(counted_t<datum_stream_t> src);
private:
    counted_t<const datum_t> next_impl(env_t *env);
};

// This has to be constructed explicitly rather than invoking `.sort()`.  There
// was a good reason for this involving header dependencies, but I don't
// remember exactly what it was.
static const size_t sort_el_limit = 1000000; // maximum number of elements we'll sort
template<class T>
class sort_datum_stream_t : public wrapper_datum_stream_t {
public:
    sort_datum_stream_t(const T &_lt_cmp, counted_t<datum_stream_t> src)
        : wrapper_datum_stream_t(src), lt_cmp(_lt_cmp), index(0) { }

    std::vector<counted_t<const datum_t> >
    next_batch_impl(env_t *env, batch_type_t batch_type) {
        std::vector<counted_t<const datum_t> > ret;
        size_t total_size = 0;
        while (!should_send_batch(ret.size(), total_size, 0)) {
            if (index >= data.size()) {
                if (ret.size() != 0 && batch_type == SINDEX_CONSTANT) {
                    // We can only load one batch of SINDEX_CONSTANT data if we
                    // want to keep the sindex constant (this should only matter
                    // if you call `.orderby(...).orderby(...)`.
                    break;
                }
                index = 0;
                data = source->next_batch(env, SINDEX_CONSTANT);
                if (data.empty()) {
                    return ret;
                }
                std::sort(data.begin(), data.end(),
                          std::bind(lt_cmp, env,
                                    std::placeholders::_1, std::placeholders::_2));
            }
            total_size += serialized_size(data[index]);
            ret.push_back(std::move(data[index]));
            index += 1;
        }
        return ret;
    }
private:
    std::function<bool(env_t *,
                       const counted_t<const datum_t> &,
                       const counted_t<const datum_t> &)> lt_cmp;

    size_t index;
    std::vector<counted_t<const datum_t> > data;
};

class union_datum_stream_t : public datum_stream_t {
public:
    union_datum_stream_t(const std::vector<counted_t<datum_stream_t> > &_streams,
                         const protob_t<const Backtrace> &bt_src)
        : datum_stream_t(bt_src), streams(_streams), streams_index(0) { }

    // stream -> stream
    virtual counted_t<datum_stream_t> filter(counted_t<func_t> f,
                                             counted_t<func_t> default_filter_val);
    virtual counted_t<datum_stream_t> map(counted_t<func_t> f);
    virtual counted_t<datum_stream_t> concatmap(counted_t<func_t> f);

    // stream -> atom
    virtual counted_t<const datum_t> count(env_t *env);
    virtual counted_t<const datum_t> reduce(env_t *env,
                                            counted_t<val_t> base_val,
                                            counted_t<func_t> f);
    virtual counted_t<const datum_t> gmr(env_t *env,
                                         counted_t<func_t> g,
                                         counted_t<func_t> m,
                                         counted_t<const datum_t> base,
                                         counted_t<func_t> r);
    virtual bool is_array();
    virtual counted_t<const datum_t> as_array(env_t *env);

private:
    counted_t<const datum_t> next_impl(env_t *env);
    std::vector<counted_t<const datum_t> >
    next_batch_impl(env_t *env, batch_type_t batch_type);

    std::vector<counted_t<datum_stream_t> > streams;
    size_t streams_index;
};

} // namespace ql

#endif // RDB_PROTOCOL_DATUM_STREAM_HPP_
