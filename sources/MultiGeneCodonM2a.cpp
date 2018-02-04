
// this is the multi-gene version of CodonM2aModel
//
// branch lengths and nucrates can be either:
// (2): shared across all genes
// (1): gene specific, with hyperparameters estimated across genes (shrinkage)
// (0): gene-specific, with fixed hyperparameters (no shrinkage); in that case, the hyperparameters are set up so as to implement vague priors
// these three alternative modes can be tuned separately for branch lengths and nuc rates
// by setting the variables blmode and nucmode to 0, 1 or 2
// this can be done using the following commands:
// -bl shared / shrunken / independent
// -nucrates shared / shrunken / independent
// by default, bl and nucrates are shared across genes.
//
// for each gene, the 3-component mixture of omega's across sites has four parameters (see CodonM2aModel.hpp):
// 0 < purom < 1
// 0 < dposom < +infty
// 0 < purw < 1
// 0 <= posw < 1
//
// these 4 parameters are always gene-specific (they cannot be shared by all genes);
// the priors over these parameters are as follows:
// purom ~ Beta(puromhypermean,puromhyperinvconc)
// dposom ~ Gamma(dposomhypermean,dposomhyperinvshape)
// purw ~ Beta purwhypermean,purwhyperinvconc)
// posw ~  mixture of point mass at 0 with prob 1-pi, and Beta(poswhypermean,poswhyperinvconc) with prob pi
//
// the 9 hyperparameters of these priors can be either:
// (1) : estimated across genes
// (0): fixed (such as given by the user)
// again, separately for each set of hyperparameters, by setting the following variables to 0 or 1: purommode, dposommode, purwmode, poswmode
//
// by default, the 9 mixture hyperparameters are estimated (shrunken) across genes (mode 1)
// the hyperpriors over these parameters are then as follows:
// Uniform(0,1) for puromhypermean, purwhypermean and poswhypermean
// Exponential(1) for puromhyperinvconc, dposomhypermean, dposomhyperinvshape, purwhyperinvconc, poswhyperinvconc
// Beta(pihypermean=0.1,pihyperinvconc=0.2) for pi
// this Beta hyperprior over can be modified by the user with this command:
// -pi <pihypermean> <pihyperinvconc>
// the Uniform and Exponential hyperpriors over the other 8 parameters cannot be modified
//
// commands for fixing the mixture hyperarams are as follows:
// -purom <puromhypermean> <puromhyperinvconc>
// -dposom <dposomhypermean> <dposomhyperinvshape>
// -purw <purwhypermean> <purwhyperinvshape>
// -posw <poswhypermean> <poswhyperinvshape>
// concerning pi, this parameter can be fixed by setting the inverse concentration of its Beta prior to 0:
// -pi <pi> 0
// in particular, fixing pi to 0 (-pi 0 0) leads to a model without positive selection
//

#include <cmath>
#include <fstream>
#include "MultiGeneCodonM2aModel.hpp"
#include "MultiGeneChain.hpp"
#include "Chrono.hpp"

using namespace std;

MPI_Datatype Propagate_arg;

class MultiGeneCodonM2aChain : public MultiGeneChain  {

  private:
    // Chain parameters
    string modeltype, datafile, treefile;
    int blmode, nucmode, purommode, dposommode, purwmode, poswmode;
    double pihypermean, pihyperinvconc;
    double puromhypermean, puromhyperinvconc;
    double dposomhypermean, dposomhyperinvshape;
    double purwhypermean, purwhyperinvconc;
    double poswhypermean, poswhyperinvconc;

  public:

    MultiGeneCodonM2aModel* GetModel() {
        return static_cast<MultiGeneCodonM2aModel*>(model);
    }

    string GetModelType() override { return modeltype; }

    MultiGeneCodonM2aChain(string indatafile, string intreefile, int inblmode, int innucmode, int inpurommode, int indposommode, int inpurwmode, int inposwmode, double inpihypermean, double inpihyperinvconc, double inpuromhypermean, double inpuromhyperinvconc, double indposomhypermean, double indposomhyperinvshape, double inpurwhypermean, double inpurwhyperinvconc, double inposwhypermean, double inposwhyperinvconc, int inevery, int inuntil, string inname, int force, int inmyid, int innprocs) : MultiGeneChain(inmyid,innprocs), modeltype("MULTIGENECODONM2A"), datafile(indatafile), treefile(intreefile) {

        blmode = inblmode;
        nucmode = innucmode;
        purommode = inpurommode;
        dposommode = indposommode;
        purwmode = inpurwmode;
        poswmode = inposwmode;
        pihypermean = inpihypermean;
        pihyperinvconc = inpihyperinvconc;
        puromhypermean = inpuromhypermean;
        puromhyperinvconc = inpuromhyperinvconc;
        dposomhypermean = indposomhypermean;
        dposomhyperinvshape = indposomhyperinvshape;
        purwhypermean = inpurwhypermean;
        purwhyperinvconc = inpurwhyperinvconc;
        poswhypermean = inposwhypermean;
        poswhyperinvconc = inposwhyperinvconc;

        every = inevery;
        until = inuntil;
        name = inname;
        New(force);
    }

    MultiGeneCodonM2aChain(string filename, int inmyid, int innprocs) : MultiGeneChain(inmyid,innprocs) {
        name = filename;
        Open();
        Save();
    }

    void New(int force) override {

        model = new MultiGeneCodonM2aModel(datafile,treefile,pihypermean,pihyperinvconc,myid,nprocs);
        GetModel()->SetAcrossGenesModes(blmode,nucmode,purommode,dposommode,purwmode,poswmode);
        GetModel()->SetMixtureHyperParameters(puromhypermean,puromhyperinvconc,dposomhypermean,dposomhyperinvshape,purwhypermean,purwhyperinvconc,poswhypermean,poswhyperinvconc);

        if (! myid) {
            cerr << "allocate\n";
        }
        GetModel()->Allocate();
        if (! myid) {
            cerr << "update\n";
        }
        GetModel()->Update();
        Reset(force);
        if (! myid) {
            cerr << "initial ln prob = " << GetModel()->GetLogProb() << "\n";
            model->Trace(cerr);
        }
    }

    void Open() override {
        ifstream is((name + ".param").c_str());
        if (!is) {
            cerr << "Error : cannot find file : " << name << ".param\n";
            exit(1);
        }
        is >> modeltype;
        is >> datafile >> treefile;
        is >> blmode >> nucmode >> dposommode >> purwmode >> poswmode;
        is >> pihypermean >> pihyperinvconc;
        is >> puromhypermean >> puromhyperinvconc;
        is >> dposomhypermean >> dposomhyperinvshape;
        is >> purwhypermean >> purwhyperinvconc;
        is >> poswhypermean >> poswhyperinvconc;

        int tmp;
        is >> tmp;
        if (tmp) {
            cerr << "Error when reading model\n";
            exit(1);
        }
        is >> every >> until >> size;

        if (modeltype == "MULTIGENECODONM2A") {
            model = new MultiGeneCodonM2aModel(datafile,treefile,pihypermean,pihyperinvconc,myid,nprocs);
            GetModel()->SetAcrossGenesModes(blmode,nucmode,purommode,dposommode,purwmode,poswmode);
            GetModel()->SetMixtureHyperParameters(puromhypermean,puromhyperinvconc,dposomhypermean,dposomhyperinvshape,purwhypermean,purwhyperinvconc,poswhypermean,poswhyperinvconc);
        } 
        else {
            cerr << "Error when opening file " << name
                 << " : does not recognise model type : " << modeltype << '\n';
            exit(1);
        }

        if (! myid) {
            cerr << "allocate\n";
        }
        GetModel()->Allocate();

        if (! myid) {
            cerr << "read from file\n";
        }
        GetModel()->FromStream(is);

        if (! myid) {
            cerr << "update\n";
        }
        GetModel()->Update();

        if (! myid) {
            cerr << size << " points saved, current ln prob = " << GetModel()->GetLogProb() << "\n";
            model->Trace(cerr);
        }
    }

    void Save() override {
        if (!myid)   {
            ofstream param_os((name + ".param").c_str());
            param_os << GetModelType() << '\n';
            param_os << datafile << '\t' << treefile << '\n';
            param_os << blmode << '\t' << nucmode << '\t' << dposommode << '\t' << purwmode << '\t' << poswmode << '\n';
            param_os << pihypermean << '\t' << pihyperinvconc << '\n';
            param_os << puromhypermean << '\t' << puromhyperinvconc << '\n';
            param_os << dposomhypermean << '\t' << dposomhyperinvshape << '\n';
            param_os << purwhypermean << '\t' << purwhyperinvconc << '\n';
            param_os << poswhypermean << '\t' << poswhyperinvconc << '\n';;
            param_os << 0 << '\n';
            param_os << every << '\t' << until << '\t' << size << '\n';
            GetModel()->MasterToStream(param_os);
        }
        else    {
            GetModel()->SlaveToStream();
        }
    }

    void MakeFiles(int force) override  {

        Chain::MakeFiles(force);

        ofstream nameos((name + ".genelist").c_str());
        GetModel()->PrintGeneList(nameos);
        nameos.close();

        ofstream pos((name + ".posw").c_str());
        ofstream omos((name + ".posom").c_str());
        /*
        if (writegenedata)  {
            ofstream siteos((name + ".sitepp").c_str());
        }
        */
    }

    void SavePoint() override   {
        Chain::SavePoint();
        if (!myid)  {
            ofstream posw_os((name + ".posw").c_str(),ios_base::app);
            GetModel()->TracePosWeight(posw_os);
            ofstream posom_os((name + ".posom").c_str(),ios_base::app);
            GetModel()->TracePosOm(posom_os);
        }
        /*
        if (!miyd && writegenedata)  {
            ofstream pp_os((name + ".sitepp").c_str(),ios_base::app);
            GetModel()->MasterTraceSitesPostProb(ppos);
            GetModel()->SlaveTraceSitesPostProb();
        }
        */
    }
};

int main(int argc, char* argv[])	{

	Chrono chrono;
    chrono.Start();

    int myid = 0;
	int nprocs = 0;

	MPI_Init(&argc,&argv);
	MPI_Comm_rank(MPI_COMM_WORLD,&myid);
	MPI_Comm_size(MPI_COMM_WORLD,&nprocs);

	int blockcounts[2] = {1,3};
	MPI_Datatype types[2] = {MPI_DOUBLE,MPI_INT};
	MPI_Aint dtex,displacements[2];
	
	displacements[0] = (MPI_Aint) 0;
	MPI_Type_extent(MPI_DOUBLE,&dtex);
	displacements[1] = dtex;
	MPI_Type_struct(2,blockcounts,displacements,types,&Propagate_arg);
	MPI_Type_commit(&Propagate_arg); 

    MultiGeneCodonM2aChain* chain = 0;
    string name = "";

    // starting a chain from existing files
    if (argc == 2 && argv[1][0] != '-') {
        name = argv[1];
        chain = new MultiGeneCodonM2aChain(name,myid,nprocs);
    }

    // new chain
    else    {
        string datafile = "";
        string treefile = "";

        double pihypermean = 0.1;
        double pihyperinvconc = 0.2;

        double puromhypermean = 0.5;
        double puromhyperinvconc = 0.5;
        int purommode = 1;

        double dposomhypermean = 1.0;
        double dposomhyperinvshape = 1.0;
        int dposommode = 1;

        double purwhypermean = 0.5;
        double purwhyperinvconc = 0.5;
        int purwmode = 1;

        double poswhypermean = 0.1;
        double poswhyperinvconc = 1;
        int poswmode = 1;

        int blmode = 2;
        int nucmode = 2;

        // int writegenedata = 0;

        int force = 1;
        int every = 1;
        int until = -1;

        try	{

            if (argc == 1)	{
                throw(0);
            }

            int i = 1;
            while (i < argc)	{
                string s = argv[i];

                if (s == "-d")	{
                    i++;
                    datafile = argv[i];
                }
                else if ((s == "-t") || (s == "-T"))	{
                    i++;
                    treefile = argv[i];
                }
                else if (s == "-purom")   {
                    purommode = 0;
                    i++;
                    string tmp = argv[i];
                    if (tmp != "uninf") {
                        puromhypermean = atof(argv[i]);
                        i++;
                        puromhyperinvconc = atof(argv[i]);
                    }
                }
                else if (s == "-dposom")    {
                    dposommode = 0;
                    i++;
                    string tmp = argv[i];
                    if (tmp != "uninf") {
                        dposomhypermean = atof(argv[i]);
                        i++;
                        dposomhyperinvshape = atof(argv[i]);
                    }
                }
                else if (s == "-purw")  {
                    purwmode = 0;
                    i++;
                    string tmp = argv[i];
                    if (tmp != "uninf") {
                        purwhypermean = atof(argv[i]);
                        i++;
                        purwhyperinvconc = atof(argv[i]);
                    }
                }
                else if (s == "-posw")  {
                    poswmode = 0;
                    i++;
                    string tmp = argv[i];
                    if (tmp != "uninf") {
                        poswhypermean = atof(argv[i]);
                        i++;
                        poswhyperinvconc = atof(argv[i]);
                    }
                }
                else if (s == "-nucrates")  {
                    i++;
                    string tmp = argv[i];
                    if (tmp == "shared")    {
                        nucmode = 2;
                    }
                    else if (tmp == "shrunken")   {
                        nucmode = 1;
                    }
                    else if ((tmp == "ind") || (tmp == "independent"))  {
                        nucmode = 0;
                    }
                    else    {
                        cerr << "error: does not recongnize command after -nucrates\n";
                        exit(1);
                    }
                }
                else if (s == "-bl")    {
                    i++;
                    string tmp = argv[i];
                    if (tmp == "shared")    {
                        blmode = 2;
                    }
                    else if (tmp == "shrunken")   {
                        blmode = 1;
                    }
                    else if ((tmp == "ind") || (tmp == "independent"))  {
                        blmode = 0;
                    }
                    else    {
                        cerr << "error: does not recongnize command after -bl\n";
                        exit(1);
                    }
                }
                else if (s == "-pi")    {
                    i++;
                    pihypermean = atof(argv[i]);
                    i++;
                    pihyperinvconc = atof(argv[i]);
                }
                /*
                else if (s == "-g")  {
                    writegenedata = 1;
                }
                */
                else if (s == "-f")	{
                    force = 1;
                }
                else if ( (s == "-x") || (s == "-extract") )	{
                    i++;
                    if (i == argc) throw(0);
                    every = atoi(argv[i]);
                    i++;
                    if (i == argc) throw(0);
                    until = atoi(argv[i]);
                }
                else	{
                    if (i != (argc -1))	{
                        throw(0);
                    }
                    name = argv[i];
                }
                i++;
            }
            if ((datafile == "") || (treefile == "") || (name == ""))	{
                throw(0);
            }
        }
        catch(...)	{
            cerr << "codonm8 -d <alignment> -t <tree> [-fixparam <paramfile> -fixhyper <hyperparamfile>] <chainname> \n";
            cerr << '\n';
            exit(1);
        }

        chain = new MultiGeneCodonM2aChain(datafile,treefile,blmode,nucmode,purommode,dposommode,purwmode,poswmode,pihypermean,pihyperinvconc,puromhypermean,puromhyperinvconc,dposomhypermean,dposomhyperinvshape,purwhypermean,purwhyperinvconc,poswhypermean,poswhyperinvconc,every,until,name,force,myid,nprocs);
    }

    chrono.Stop();
    if (! myid) {
        cout << "total time to set things up: " << chrono.GetTime() << '\n';
    }
    chrono.Reset();
    chrono.Start();
    if (! myid) {
        cerr << "chain " << name << " started\n";
    }
    chain->Start();
    if (! myid) {
        cerr << "chain " << name << " stopped\n";
        cerr << chain->GetSize() << "-- Points saved, current ln prob = " << chain->GetModel()->GetLogProb() << "\n";
        chain->GetModel()->Trace(cerr);
    }
    chrono.Stop();
    if (! myid) {
        cout << "total time to run: " << chrono.GetTime() << '\n';
        cout << "total time in master moves: " << chain->GetModel()->GetMasterMoveTime() << '\n';
        cout << "mean total time in slave moves: " << chain->GetModel()->GetSlaveMoveTime() << '\n';
        cout << "mean total time in substitution mapping: " << chain->GetModel()->GetSlaveMapTime() << '\n';
    }

	MPI_Finalize();
}

