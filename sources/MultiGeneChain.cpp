#include "MultiGeneChain.hpp"
#include <fstream>
#include <iostream>
#include "Chrono.hpp"
#include "MultiGeneProbModel.hpp"
#include "monitoring.hpp"
#include <list>

std::unique_ptr<MonitorManager> gm(new MonitorManager());

using namespace std;

// c++11
#define nullptr 0

MultiGeneChain::MultiGeneChain(int inmyid, int innprocs)
    : Chain(), myid(inmyid), nprocs(innprocs) {}

void MultiGeneChain::SavePoint() {
    if (saveall) {
        if (!myid) {
            ofstream chain_os((name + ".chain").c_str(), ios_base::app);
            GetMultiGeneModel()->MasterToStream(chain_os);
        } else {
            GetMultiGeneModel()->SlaveToStream();
        }
    }
    size++;
}

void MultiGeneChain::Reset(int force) {
    size = 0;
    if (!myid) {
        MakeFiles(force);
    }
    Save();
}

void MultiGeneChain::MakeFiles(int force) {
    Chain::MakeFiles(force);
    ofstream nameos((name + ".genelist").c_str());
    GetMultiGeneModel()->PrintGeneList(nameos);
    nameos.close();
}

void MultiGeneChain::Move() {
    for (int i = 0; i < every; i++) {
        GetMultiGeneModel()->Move();
    }
    SavePoint();
    Save();
    if (!myid) {
        Monitor();
    }
}

void MultiGeneChain::Start() {
    if (!myid) {
        ofstream run_os((name + ".run").c_str());
        run_os << 1 << '\n';
        run_os.close();
    }
    Run();
}

void MultiGeneChain::MasterSendRunningStatus(int status) {
    MPI_Bcast(&status, 1, MPI_INT, 0, MPI_COMM_WORLD);
}

int MultiGeneChain::SlaveReceiveRunningStatus() {
    int status;
    MPI_Bcast(&status, 1, MPI_INT, 0, MPI_COMM_WORLD);
    return status;
}

void MultiGeneChain::Run() {
    ofstream mvfile(name + "_p" + to_string(myid) + "_" + to_string(size) + "to" +
                    to_string(until) + ".movestats");

    int first_iteration = size + 1;

    auto write_line = [this, &mvfile]() {
        stringstream line;
        for (auto& monitor : gm->monitors) {
            if (line.str() != "") {
                line << '\t';
            }
            line << dynamic_cast<MeanMonitor<double>*>(monitor.second.get())->tmp_mean();
            dynamic_cast<MeanMonitor<double>*>(monitor.second.get())->tmp_reset();
        }
        mvfile << line.str() << "\n";
    };

    auto write_header = [this, &mvfile]() {
        stringstream header;
        for (auto& monitor : gm->monitors) {
            if (header.str() != "") {
                header << '\t';
            }
            header << monitor.first;
        }
        mvfile << header.str() << "\n";
    };

    list<double> last_it_times;
    double max_time = 24 * 60 * 60 * 1000; // 24 hours
    // double max_time = 3 * 60 * 1000; // 3 minutes

    auto mean_and_trim = [this, &last_it_times] (double it_time) -> double {
        last_it_times.push_back(it_time);
        double sum = 0;
        int count = 0;
        for (auto t : last_it_times) {
            count ++;
            sum += t;
        }
        if (last_it_times.size() > 5) {
            last_it_times.pop_front();
        }
        return sum / count;
    };

    //=============================================================================================
    //  MAIN LOOP
    //=============================================================================================
    if (!myid) {
        Chrono total_time_chrono;
        total_time_chrono.Start();
        while ((GetRunningStatus() != 0) && ((until == -1) || (size <= until))) {
            MasterSendRunningStatus(1);
            Chrono chrono;
            chrono.Start();
            Move();
            chrono.Stop();

            double it_time = chrono.GetTime();
            double mean_it_time = mean_and_trim(it_time);
            total_time_chrono.Stop();
            double total_time = total_time_chrono.GetTime();
            int remaining_its = floor((max_time - total_time) / mean_it_time);

            ofstream check_os((name + ".time").c_str());
            check_os << it_time << '\n';
            cerr << "* Iteration " << size - 1 << ": " << it_time / 1000
                 << "s (mean it time: " << mean_it_time / 1000 << "s; predicting "
                 << remaining_its - 2 << " more iterations)\n";

            if (size == first_iteration) {
                write_header();
            }
            write_line();

            if (total_time + 3 * mean_it_time > max_time) {
                printf("Appraoching max time! Stopping computation.\n");
                break;
            }
        }
        MasterSendRunningStatus(0);
        ofstream run_os((name + ".run").c_str());
        run_os << 0 << '\n';
    } else {
        while (SlaveReceiveRunningStatus()) {
            Move();
            write_line();
        }
    }
}
