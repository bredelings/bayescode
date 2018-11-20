#pragma once

#include <functional>
#include "Process.hpp"
#include "components/RegistrarBase.hpp"
#include "interfaces.hpp"

/*==================================================================================================
  ReducerMaster
  An object responsible for Reducing the values of specified fields to other processes
  is meant to communicate with one ReducerSlave per other process
==================================================================================================*/
template <typename T>
class ReducerMaster : public Proxy, public RegistrarBase<ReducerMaster<T>> {
    using buf_it = typename std::vector<T>::iterator;

    Process& p;             // to get rank and size
    MPI_Datatype datatype;  // mpi datatype of all managed elements
    std::vector<T> buf;     // buffer to be used by MPI
    std::vector<T> zeroes;  // contribution of master

    // These functions are responsible for reading data from buffer after the reduce and to write it
    // to the corresponding variables.
    std::vector<std::function<void(buf_it&)>> readers;

    friend RegistrarBase<ReducerMaster<T>>;
    using RegistrarBase<ReducerMaster<T>>::register_element;

    void register_element(std::string, T& target) {
        readers.push_back([&target](buf_it& it) {
            target = *it;
            it++;
        });
        buf.push_back(-1);  // so buf has the right size
        zeroes.push_back(0);
    }

    void register_element(std::string, std::vector<T>& target) {
        readers.push_back([&target](buf_it& it) {
            target = std::vector<T>(it, it + target.size());
            it += target.size();
        });
        buf.insert(buf.end(), std::vector<T>(target.size(), -1));  // so buf has the right size
        zeroes.insert(buf.end(), std::vector<T>(target.size(), 0));
    }

    void read_buffer() {
        buf_it it = buf.begin();
        for (auto reader : readers) { reader(it); }
    }

  public:
    ReducerMaster(Process& p = *MPI::p) : p(p), datatype(get_datatype<T>()) {}

    void acquire() final {
        MPI_Reduce(
            zeroes.data(), buf.data(), buf.size(), datatype, MPI_SUM, p.rank, MPI_COMM_WORLD);
        read_buffer();
    }
};

/*==================================================================================================
  ReducerSlave
==================================================================================================*/
template <typename T>
class ReducerSlave : public Proxy, public RegistrarBase<ReducerSlave<T>> {
    Process& p;             // to get rank and size
    int origin;             // reduce operation origin (typically 0)
    MPI_Datatype datatype;  // mpi datatype of all managed elements
    std::vector<T> buf;     // buffer to be used by MPI

    // Functions that add data to buffer
    std::vector<std::function<void()>> writers;

    friend RegistrarBase<ReducerSlave<T>>;
    using RegistrarBase<ReducerSlave<T>>::register_element;

    void register_element(std::string, T& target) {
        writers.push_back([&target, this]() { buf.push_back(target); });
    }

    void register_element(std::string, std::vector<T>& target) {
        writers.push_back([&target, this]() { buf.insert(buf.end(), target); });
    }

    void write_buffer() {
        buf.clear();
        for (auto writer : writers) { writer(); }
    }

  public:
    ReducerSlave(int origin = 0, Process& p = *MPI::p)
        : p(p), origin(0), datatype(get_datatype<T>()) {}

    void release() final {
        write_buffer();
        MPI_Reduce(buf.data(), NULL, buf.size(), datatype, MPI_SUM, origin, MPI_COMM_WORLD);
    }
};

/*==================================================================================================
  Reduce functions
  Functions that are meant to be called globally and that will create either a master or slave
  component depending on the process
==================================================================================================*/
template <class Model, class T = double>
std::unique_ptr<Proxy> reduce(Model& m, void (Model::*f_master)(RegistrarBase<ReducerMaster<T>>&),
    void (Model::*f_slave)(RegistrarBase<ReducerSlave<T>>&),
    std::set<std::string> filter = std::set<std::string>{}) {
    std::unique_ptr<Proxy> result{nullptr};
    if (!MPI::p->rank) {
        auto component = new ReducerMaster<T>();
        component->register_from_method(m, f_master, filter);
        result.reset(dynamic_cast<Proxy*>(component));
    } else {
        auto component = new ReducerSlave<T>();
        component->register_from_method(m, f_slave, filter);
        result.reset(dynamic_cast<Proxy*>(component));
    }
    return result;
}

template <class Model, class T = double>
std::unique_ptr<Proxy> reduce_model(
    Model& m, std::set<std::string> filter = std::set<std::string>{}) {
    return reduce(m, &Model::declare_model, &Model::declare_model, filter);
}