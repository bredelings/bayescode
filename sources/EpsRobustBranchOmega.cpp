#include <cmath>
#include <fstream>
#include "Chain.hpp"
#include "EpsRobustBranchOmegaModel.hpp"
using namespace std;

/**
 * \brief Chain object for running an MCMC under EpsRobustBranchOmegaModel
 */

class EpsRobustBranchOmegaChain : public Chain {
  private:
    // Chain parameters
    string modeltype;
    string datafile, treefile;
    double epsilon;

  public:
    //! constructor for a new chain: datafile, treefile, saving frequency, final
    //! chain size, chain name and overwrite flag -- calls New
    EpsRobustBranchOmegaChain(string indatafile, string intreefile, double inepsilon, int inevery, int inuntil, string inname,
                     int force)
        : modeltype("EPSROBUSTBRANCHOMEGA"), datafile(indatafile), treefile(intreefile), epsilon(inepsilon) {
        every = inevery;
        until = inuntil;
        name = inname;
        New(force);
    }

    //! constructor for re-opening an already existing chain from file -- calls
    //! Open
    EpsRobustBranchOmegaChain(string filename) {
        name = filename;
        Open();
        Save();
    }

    void New(int force) override {
        model = new EpsRobustBranchOmegaModel(datafile, treefile, epsilon);
        GetModel()->Allocate();
        GetModel()->Update();
        cerr << "-- Reset" << endl;
        Reset(force);
        cerr << "-- initial ln prob = " << GetModel()->GetLogProb() << "\n";
        model->Trace(cerr);
    }

    void Open() override {
        ifstream is((name + ".param").c_str());
        if (!is) {
            cerr << "-- Error : cannot find file : " << name << ".param\n";
            exit(1);
        }
        is >> modeltype;
        is >> datafile >> treefile;
        is >> epsilon;
        int tmp;
        is >> tmp;
        if (tmp) {
            cerr << "-- Error when reading model\n";
            exit(1);
        }
        is >> every >> until >> size;

        if (modeltype == "EPSROBUSTBRANCHOMEGA") {
            model = new EpsRobustBranchOmegaModel(datafile, treefile, epsilon);
        } else {
            cerr << "-- Error when opening file " << name
                 << " : does not recognise model type : " << modeltype << '\n';
            exit(1);
        }
        GetModel()->Allocate();
        model->FromStream(is);
        model->Update();
        cerr << size << " points saved, current ln prob = " << GetModel()->GetLogProb() << "\n";
        model->Trace(cerr);
    }

    void Save() override {
        ofstream param_os((name + ".param").c_str());
        param_os << GetModelType() << '\n';
        param_os << datafile << '\t' << treefile << '\n';
        param_os << epsilon << '\n';
        param_os << 0 << '\n';
        param_os << every << '\t' << until << '\t' << size << '\n';
        model->ToStream(param_os);
    }

    void SavePoint() override {
        Chain::SavePoint();
        ofstream pos((name + ".branchomega").c_str(), ios_base::app);
        GetModel()->TraceEpsRobustBranchOmega(pos);
    }

    void MakeFiles(int force) override {
        Chain::MakeFiles(force);
        ofstream pos((name + ".branchomega").c_str());
    }

    //! return the model, with its derived type (unlike ProbModel::GetModel)
    EpsRobustBranchOmegaModel *GetModel() { return static_cast<EpsRobustBranchOmegaModel *>(model); }

    //! return model type
    string GetModelType() override { return modeltype; }
};

int main(int argc, char *argv[]) {
    string name = "";
    EpsRobustBranchOmegaChain *chain = 0;

    // starting a chain from existing files
    if (argc == 2 && argv[1][0] != '-') {
        name = argv[1];
        chain = new EpsRobustBranchOmegaChain(name);
    }

    // new chain
    else {
        string datafile = "";
        string treefile = "";
        double epsilon = 0.05;
        name = "";
        int force = 1;
        int every = 1;
        int until = -1;

        try {
            if (argc == 1) {
                throw(0);
            }

            int i = 1;
            while (i < argc) {
                string s = argv[i];

                if (s == "-d") {
                    i++;
                    datafile = argv[i];
                } else if ((s == "-t") || (s == "-T")) {
                    i++;
                    treefile = argv[i];
                } else if (s == "-f") {
                    force = 1;
                } else if (s == "-eps") {
                    i++;
                    epsilon = atof(argv[i]);
                } else if ((s == "-x") || (s == "-extract")) {
                    i++;
                    if (i == argc) throw(0);
                    every = atoi(argv[i]);
                    i++;
                    if (i == argc) throw(0);
                    until = atoi(argv[i]);
                } else {
                    if (i != (argc - 1)) {
                        throw(0);
                    }
                    name = argv[i];
                }
                i++;
            }
            if ((datafile == "") || (treefile == "") || (name == "")) {
                throw(0);
            }
        } catch (...) {
            cerr << "globom -d <alignment> -t <tree> <chainname> \n";
            cerr << '\n';
            exit(1);
        }

        chain = new EpsRobustBranchOmegaChain(datafile, treefile, epsilon, every, until, name, force);
    }

    cerr << "chain " << name << " started\n";
    chain->Start();
    cerr << "chain " << name << " stopped\n";
    cerr << chain->GetSize()
         << "-- Points saved, current ln prob = " << chain->GetModel()->GetLogProb() << "\n";
    chain->GetModel()->Trace(cerr);
}
