
// this is a multigene version of singleomegamodel
//
// - branch lengths are shared across genes, and are iid Exponential of rate
// lambda
// - nucleotide relative exchangeabilities and stationaries are also shared
// across genes (uniform Dirichlet)
// - the array of gene-specific omega's are iid gamma with hyperparameters
// omegahypermean and omegahyperinvshape
//
// the sequence of MCMC moves is as follows:
// - genes resample substitution histories, gather path suff stats and move
// their omega's
// - master receives the array of omega's across genes, moves their
// hyperparameters and then broadcast the new value of these hyperparams
// - master collects branch length suff stats across genes, moves branch lengths
// and broadcasts their new value
// - master collects nuc path suffstats across genes, moves nuc rates and
// broadcasts their new value

#include "MultiGeneProbModel.hpp"
#include "Parallel.hpp"
#include "SiteOmegaModel.hpp"
#include "IIDDirichlet.hpp"
#include "IIDGamma.hpp"

class MultiGeneSiteOmegaModel : public MultiGeneProbModel {

  private:

    Tree *tree;
    CodonSequenceAlignment *refcodondata;
    const TaxonSet *taxonset;
    std::vector<CodonSequenceAlignment*> alivector;

    string datapath;
    string datafile;
    string treefile;

    int Ntaxa;
    int Nbranch;

    int blmode;
    int nucmode;
    int omegamode;

    // Branch lengths

    double lambda;
    BranchIIDGamma *branchlength;
    GammaSuffStat hyperlengthsuffstat;

    double blhyperinvshape;
    GammaWhiteNoiseArray *branchlengtharray;
    PoissonSuffStatBranchArray *lengthpathsuffstatarray;
    GammaSuffStatBranchArray *lengthhypersuffstatarray;

    // Nucleotide rates

    // shared nuc rates
    GTRSubMatrix *nucmatrix;
    NucPathSuffStat nucpathsuffstat;

    // gene-specific nuc rates
    vector<double> nucrelratehypercenter;
    double nucrelratehyperinvconc;
    IIDDirichlet *nucrelratearray;
    DirichletSuffStat nucrelratesuffstat;

    vector<double> nucstathypercenter;
    double nucstathyperinvconc;
    IIDDirichlet *nucstatarray;
    DirichletSuffStat nucstatsuffstat;

    // each gene defines its own SiteOmegaModel
    std::vector<SiteOmegaModel *> geneprocess;

    // total log likelihood (summed across all genes)
    double lnL;
    // total logprior for gene-specific variables (here, omega only)
    // summed over all genes
    double GeneLogPrior;

    double omegameanhypermean, omegameanhyperinvshape;
    IIDGamma *omegameanarray;
    GammaSuffStat omegameanhypersuffstat;

    double omegainvshapehypermean, omegainvshapehyperinvshape;
    IIDGamma *omegainvshapearray;
    GammaSuffStat omegainvshapehypersuffstat;

    SimpleArray<double>* omegaarray;

  public:
    //-------------------
    // Construction and allocation
    //-------------------

    MultiGeneSiteOmegaModel(string indatafile, string intreefile, int inmyid, int innprocs)
        : MultiGeneProbModel(inmyid, innprocs),
          nucrelratesuffstat(Nrr),
          nucstatsuffstat(Nnuc) {

        blmode = 1;
        nucmode = 1;
        omegamode = 1;

        datafile = indatafile;
        treefile = intreefile;
        AllocateAlignments(datafile);

        refcodondata = new CodonSequenceAlignment(refdata, true);
        taxonset = refdata->GetTaxonSet();
        Ntaxa = refdata->GetNtaxa();

        // get tree from file (newick format)
        tree = new Tree(treefile);

        // check whether tree and data fits together
        tree->RegisterWith(taxonset);

        tree->SetIndices();
        Nbranch = tree->GetNbranch();

        if (!myid) {
            cerr << "number of taxa : " << Ntaxa << '\n';
            cerr << "number of branches : " << Nbranch << '\n';
            cerr << "tree and data fit together\n";
        }
    }

    void Allocate() {

        // Branch lengths

        lambda = 10;
        branchlength = new BranchIIDGamma(*tree, 1.0, lambda);
        blhyperinvshape = 0.1;
        if (blmode == 2) {
            lengthpathsuffstatarray = new PoissonSuffStatBranchArray(*tree);
            lengthhypersuffstatarray = 0;
        } else {
            branchlength->SetAllBranches(1.0 / lambda);
            branchlengtharray =
                new GammaWhiteNoiseArray(GetLocalNgene(), *tree, *branchlength, 1.0 / blhyperinvshape);
            lengthpathsuffstatarray = 0;
            lengthhypersuffstatarray = new GammaSuffStatBranchArray(*tree);
        }

        // Nucleotide rates

        nucrelratehypercenter.assign(Nrr, 1.0 / Nrr);
        nucrelratehyperinvconc = 0.1 / Nrr;

        nucstathypercenter.assign(Nnuc, 1.0 / Nnuc);
        nucstathyperinvconc = 0.1 / Nnuc;

        if (nucmode == 2) {
            nucrelratearray = new IIDDirichlet(1, nucrelratehypercenter, 1.0 / nucrelratehyperinvconc);
            nucstatarray = new IIDDirichlet(1, nucstathypercenter, 1.0 / nucstathyperinvconc);
            nucmatrix = new GTRSubMatrix(Nnuc, (*nucrelratearray)[0], (*nucstatarray)[0], true);
        } else {
            nucrelratearray =
                new IIDDirichlet(GetLocalNgene(), nucrelratehypercenter, 1.0 / nucrelratehyperinvconc);
            nucstatarray =
                new IIDDirichlet(GetLocalNgene(), nucstathypercenter, 1.0 / nucstathyperinvconc);
            nucmatrix = 0;
        }

        // Gene processes 

        lnL = 0;
        GeneLogPrior = 0;

        omegameanarray = new IIDGamma(GetLocalNgene(), omegameanhypermean, omegameanhyperinvshape);
        omegainvshapearray = new IIDGamma(GetLocalNgene(), omegainvshapehypermean, omegainvshapehyperinvshape);

        omegaarray = new SimpleArray<double>(GetLocalNgene());

        if (!GetMyid()) {
            geneprocess.assign(0, (SiteOmegaModel *)0);
        } else {
            geneprocess.assign(GetLocalNgene(), (SiteOmegaModel *)0);

            ifstream is(datafile.c_str());
            string tmp;
            is >> tmp;
            if (tmp == "ALI")   {
                int ngene;
                is >> ngene;
                if (ngene != GetNgene())    {
                    cerr << "error when reading alignments from cat file: non matching number of genes\n";
                    exit(1);
                }
                alivector.assign(GetLocalNgene(), (CodonSequenceAlignment*) 0);
                int index = 0;
                for (int gene=0; gene<GetNgene(); gene++)   {
                    string name;
                    is >> name;
                    FileSequenceAlignment tmp(is);
                    if (name == GeneName[index])    {
                        if (GetLocalGeneName(index) != name)    {
                            cerr << "error: non matching gene name\n";
                            exit(1);
                        }
                        if (alivector[index]) {
                            cerr << "error: alignment already allocated\n";
                            exit(1);
                        }
                        alivector[index] = new CodonSequenceAlignment(&tmp, true);
                        index++;
                    }
                }
                for (int gene = 0; gene < GetLocalNgene(); gene++) {
                    if (! alivector[gene])  {
                        cerr << "error: alignment not allocated\n";
                        exit(1);
                    }
                    geneprocess[gene] = new SiteOmegaModel(alivector[gene], tree);
                }
            }
            else    {
                for (int gene = 0; gene < GetLocalNgene(); gene++) {
                    geneprocess[gene] = new SiteOmegaModel(GetLocalGeneName(gene), treefile);
                }
            }

            for (int gene = 0; gene < GetLocalNgene(); gene++) {
                geneprocess[gene]->SetAcrossGenesModes(blmode, nucmode);
                geneprocess[gene]->Allocate();
            }
        }
    }

    // called upon constructing the model
    // mode == 2: global 
    // mode == 1: gene specific, with hyperparameters estimated across genes
    // mode == 0: gene-specific, with fixed hyperparameters
    void SetAcrossGenesModes(int inblmode, int innucmode, int inomegamode)   {
        blmode = inblmode;
        nucmode = innucmode;
        omegamode = inomegamode;
    }

    void SetOmegaHyperParameters(double inomegameanhypermean, double inomegameanhyperinvshape, double inomegainvshapehypermean, double inomegainvshapehyperinvshape)    {
        omegameanhypermean = inomegameanhypermean;
        omegameanhyperinvshape = inomegameanhyperinvshape;
        omegainvshapehypermean = inomegainvshapehypermean;
        omegainvshapehyperinvshape = inomegainvshapehyperinvshape;
    }

    void FastUpdate() {

        branchlength->SetScale(lambda);
        if (blmode == 1) {
            branchlengtharray->SetShape(1.0 / blhyperinvshape);
        }
        nucrelratearray->SetConcentration(1.0 / nucrelratehyperinvconc);
        nucstatarray->SetConcentration(1.0 / nucstathyperinvconc);

        double meanalpha = 1.0 / omegameanhyperinvshape;
        double meanbeta = meanalpha / omegameanhypermean;
        omegameanarray->SetShape(meanalpha);
        omegameanarray->SetScale(meanbeta);

        double invshapealpha = 1.0 / omegainvshapehyperinvshape;
        double invshapebeta = invshapealpha / omegainvshapehypermean;
        omegainvshapearray->SetShape(invshapealpha);
        omegainvshapearray->SetScale(invshapebeta);
    }

    void MasterUpdate() override {

        FastUpdate();

        if (nprocs > 1) {
            MasterSendBranchLengthsHyperParameters();
            MasterSendNucRatesHyperParameters();

            if (blmode == 2) {
                MasterSendGlobalBranchLengths();
            } else {
                MasterSendGeneBranchLengths();
            }

            if (nucmode == 2) {
                MasterSendGlobalNucRates();
            } else {
                MasterSendGeneNucRates();
            }

            MasterSendOmegaHyperParameters();
            MasterSendOmegaParameters();

            MasterReceiveLogProbs();
        }
    }

    void SlaveUpdate() override {

        SlaveReceiveBranchLengthsHyperParameters();
        SlaveReceiveNucRatesHyperParameters();

        if (blmode == 2) {
            SlaveReceiveGlobalBranchLengths();
        } else {
            SlaveReceiveGeneBranchLengths();
        }
        if (nucmode == 2) {
            SlaveReceiveGlobalNucRates();
        } else {
            SlaveReceiveGeneNucRates();
        }

        SlaveReceiveOmegaHyperParameters();
        SlaveReceiveOmegaParameters();

        GeneUpdate();
        SlaveSendLogProbs();
    }

    void GeneUpdate() {
        for (int gene = 0; gene < GetLocalNgene(); gene++) {
            geneprocess[gene]->Update();
        }
    }

    void MasterPostPred(string name) override {
        FastUpdate();
        if (nprocs > 1) {
            MasterSendBranchLengthsHyperParameters();
            MasterSendNucRatesHyperParameters();

            if (blmode == 2) {
                MasterSendGlobalBranchLengths();
            } else {
                MasterSendGeneBranchLengths();
            }

            if (nucmode == 2) {
                MasterSendGlobalNucRates();
            } else {
                MasterSendGeneNucRates();
            }

            MasterSendOmegaHyperParameters();
            MasterSendOmegaParameters();
        }
    }

    void SlavePostPred(string name) override {
        SlaveReceiveBranchLengthsHyperParameters();
        SlaveReceiveNucRatesHyperParameters();

        if (blmode == 2) {
            SlaveReceiveGlobalBranchLengths();
        } else {
            SlaveReceiveGeneBranchLengths();
        }
        if (nucmode == 2) {
            SlaveReceiveGlobalNucRates();
        } else {
            SlaveReceiveGeneNucRates();
        }

        SlaveReceiveOmegaHyperParameters();
        SlaveReceiveOmegaParameters();

        GenePostPred(name);
    }

    void GenePostPred(string name) {
        for (int gene = 0; gene < GetLocalNgene(); gene++) {
            geneprocess[gene]->PostPred(name + GetLocalGeneName(gene));
        }
    }

    CodonStateSpace *GetCodonStateSpace() const {
        return (CodonStateSpace *)refcodondata->GetStateSpace();
    }

    //-------------------
    // Traces and Monitors
    //-------------------

    void TraceHeader(ostream &os) const override {
        os << "#logprior\tlnL";
        if (blmode == 2) {
            os << "\tlength";
        } else {
            os << "\tmeanlength\tstdev";
        }
        os << "\tmeanom\tvarom";
        os << "\tmeanhypermean\tmeanhyperinvshape";
        os << "\tinvshapehypermean\tinvshapehyperinvshape";
        os << "\tstatent";
        os << "\trrent";
        if (nucmode != 2) {
            os << "\tstdevrr\tcenter\thyperinvconc";
            os << "\tstdevstat\tcenter\thyperinvconc";
        }
        os << '\n';
    }

    void Trace(ostream &os) const override {
        os << GetLogPrior() << '\t';
        os << GetLogLikelihood();

        if (blmode == 2) {
            os << '\t' << GetMeanTotalLength();
        } else {
            os << '\t' << GetMeanLength();
            os << '\t' << sqrt(GetVarLength());
        }

        os << '\t' << GetMeanOmega() << '\t' << GetVarOmega();
        os << '\t' << omegameanhypermean << '\t' << omegameanhyperinvshape;
        os << '\t' << omegainvshapehypermean << '\t' << omegainvshapehyperinvshape;

        os << '\t' << nucstatarray->GetMeanEntropy();
        os << '\t' << nucrelratearray->GetMeanEntropy();
        if (nucmode != 2) {
            os << '\t' << sqrt(GetVarNucRelRate()) << '\t' << Random::GetEntropy(nucrelratehypercenter)
               << '\t' << nucrelratehyperinvconc;
            os << '\t' << sqrt(GetVarNucStat()) << '\t' << Random::GetEntropy(nucstathypercenter)
               << '\t' << nucstathyperinvconc;
        }
        os << '\n';
        os.flush();
    }

    double GetMeanOmega() const {
        double m1 = 0;
        for (int gene=0; gene<GetLocalNgene(); gene++)  {
            m1 += omegaarray->GetVal(gene);
        }
        m1 /= GetLocalNgene();
        return m1;
    }

    double GetVarOmega() const {
        double m1 = 0;
        double m2 = 0;
        for (int gene=0; gene<GetLocalNgene(); gene++)  {
            m1 += omegaarray->GetVal(gene);
            m2 += omegaarray->GetVal(gene) * omegaarray->GetVal(gene);
        }
        m1 /= GetLocalNgene();
        m2 /= GetLocalNgene();
        m2 -= m1*m1;
        return m2;
    }

    // Branch lengths

    double GetMeanTotalLength() const {
        double tot = 0;
        for (int j = 0; j < Nbranch; j++) {
            tot += branchlength->GetVal(j);
        }
        return tot;
    }

    double GetMeanLength() const {
        if (blmode == 2) {
            cerr << "error: in getvarlength\n";
            exit(1);
        }

        return branchlengtharray->GetMeanLength();
    }

    double GetVarLength() const {
        if (blmode == 2) {
            cerr << "error: in getvarlength\n";
            exit(1);
        }

        return branchlengtharray->GetVarLength();
    }

    // Nucleotide rates

    double GetVarNucRelRate() const {
        if (nucmode == 2) {
            cerr << "error in getvarnucrelrate\n";
            exit(1);
        }

        double tot = 0;
        for (int j = 0; j < Nrr; j++) {
            double mean = 0;
            double var = 0;
            for (int g = 0; g < Ngene; g++) {
                double tmp = (*nucrelratearray)[g][j];
                mean += tmp;
                var += tmp * tmp;
            }
            mean /= Ngene;
            var /= Ngene;
            var -= mean * mean;
            tot += var;
        }
        tot /= Nrr;
        return tot;
    }

    double GetVarNucStat() const {
        if (nucmode == 2) {
            cerr << "error in getvarnucstat\n";
            exit(1);
        }

        double tot = 0;
        for (int j = 0; j < Nnuc; j++) {
            double mean = 0;
            double var = 0;
            for (int g = 0; g < Ngene; g++) {
                double tmp = (*nucstatarray)[g][j];
                mean += tmp;
                var += tmp * tmp;
            }
            mean /= Ngene;
            var /= Ngene;
            var -= mean * mean;
            tot += var;
        }
        tot /= Nnuc;
        return tot;
    }

    void Monitor(ostream &os) const override {}

    void MasterFromStream(istream &is) override {

        if (blmode == 2) {
            is >> lambda;
            is >> *branchlength;
        } else {
            is >> lambda;
            is >> *branchlength;
            is >> blhyperinvshape;
            is >> *branchlengtharray;
        }

        is >> nucrelratehypercenter;
        is >> nucrelratehyperinvconc;
        is >> nucstathypercenter;
        is >> nucstathyperinvconc;
        is >> *nucrelratearray;
        is >> *nucstatarray;

        is >> omegameanhypermean >> omegameanhyperinvshape;
        is >> *omegameanarray;
        is >> omegainvshapehypermean >> omegainvshapehyperinvshape;
        is >> *omegainvshapearray;
    }

    void MasterToStream(ostream &os) const override {

        if (blmode == 2) {
            os << lambda << '\t';
            os << *branchlength << '\t';
        } else {
            os << lambda << '\t';
            os << *branchlength << '\t';
            os << blhyperinvshape << '\t';
            os << *branchlengtharray << '\t';
        }

        os << nucrelratehypercenter << '\t';
        os << nucrelratehyperinvconc << '\t';
        os << nucstathypercenter << '\t';
        os << nucstathyperinvconc << '\t';
        os << *nucrelratearray << '\t';
        os << *nucstatarray << '\t';

        os << omegameanhypermean << '\t' << omegameanhyperinvshape << '\t';
        os << *omegameanarray << '\t';
        os << omegainvshapehypermean << '\t' << omegainvshapehyperinvshape << '\t';
        os << *omegainvshapearray << '\t';
    }

    void TraceOmega(ostream &os) const {
        for (int gene = 0; gene < Ngene; gene++) {
            os << omegaarray->GetVal(gene) << '\t';
        }
        os << '\n';
        os.flush();
    }

    void TracedS(ostream &os) const {
        for (int gene = 0; gene < Ngene; gene++) {
            os << branchlengtharray->GetVal(gene).GetTotalLength() << '\t';
        }
        os << '\n';
        os.flush();
    }

    void MasterTraceSiteOmega(ostream &os) {
        for (int proc = 1; proc < GetNprocs(); proc++) {
            int totnsite = GetSlaveTotNsite(proc);
            double *array = new double[totnsite];
            MPI_Status stat;
            MPI_Recv(array, totnsite, MPI_DOUBLE, proc, TAG1, MPI_COMM_WORLD, &stat);

            int i = 0;
            for (int gene = 0; gene < Ngene; gene++) {
                if (GeneAlloc[gene] == proc) {
                    os << GeneName[gene] << '\t';
                    int nsite = GeneNsite[gene];
                    for (int k = 0; k < nsite; k++) {
                        os << array[i++] << '\t';
                    }
                }
            }
            if (i != totnsite) {
                cerr << "error in MultiGeneCodonM2aModel::MasterTraceSiteOmega: non "
                        "matching number of sites\n";
                exit(1);
            }
            delete[] array;
        }
        os << '\n';
        os.flush();
    }

    void SlaveTraceSiteOmega() {
        int ngene = GetLocalNgene();
        int totnsite = GetLocalTotNsite();
        double *array = new double[totnsite];
        int i = 0;
        for (int gene = 0; gene < ngene; gene++) {
            geneprocess[gene]->GetSiteOmega(array + i);
            for (int j = 0; j < GeneNsite[gene]; j++) {
                if (array[i + j] < 0) {
                    cerr << "error in slave\n";
                    cerr << i << '\t' << j << '\t' << GeneName[gene] << '\t' << GeneNsite[gene] << '\t'
                         << geneprocess[gene]->GetNsite() << '\n';
                    exit(1);
                }
            }
            i += GetLocalGeneNsite(gene);
        }
        if (i != totnsite) {
            cerr << "error in MultiGeneCodonM2aModel::SlaveTraceSiteOmega: non "
                    "matching number of sites\n";
            exit(1);
        }

        MPI_Send(array, totnsite, MPI_DOUBLE, 0, TAG1, MPI_COMM_WORLD);
        delete[] array;
    }

    //-------------------
    // Updates
    //-------------------

    void UpdateNucMatrix() {
        nucmatrix->CopyStationary((*nucstatarray)[0]);
        nucmatrix->CorruptMatrix();
    }

    void NoUpdate() {}

    //-------------------
    // Log Prior and Likelihood
    //-------------------

    double GetLogPrior() const {
        // gene contributions
        double total = GeneLogPrior;

        // branch lengths
        if (blmode == 2) {
            total += GlobalBranchLengthsLogPrior();
        } else if (blmode == 1) {
            total += GeneBranchLengthsHyperLogPrior();
        } else {
            // nothing: everything accounted for by gene component
        }

        // nuc rates
        if (nucmode == 2) {
            total += GlobalNucRatesLogPrior();
        } else if (nucmode == 1) {
            total += GeneNucRatesHyperLogPrior();
        } else {
            // nothing: everything accounted for by gene component
        }

        if (omegamode == 1) {
            total += OmegaHyperLogPrior();
        }
        // already accounted for in GeneLogPrior
        // total += OmegaLogPrior();

        return total;
    }

    // Branch lengths

    double LambdaHyperLogPrior() const { return -lambda / 10; }

    double GlobalBranchLengthsLogPrior() const {
        return LambdaHyperLogPrior() + branchlength->GetLogProb();
    }

    // exponential of mean 1 for blhyperinvshape
    double BranchLengthsHyperInvShapeLogPrior() const { return -blhyperinvshape; }

    double GeneBranchLengthsHyperLogPrior() const {
        return BranchLengthsHyperInvShapeLogPrior() + branchlength->GetLogProb();
    }

    // Nucleotide rates

    double GlobalNucRatesLogPrior() const {
        return nucrelratearray->GetLogProb() + nucstatarray->GetLogProb();
    }

    // exponential of mean 1 for nucrelrate and nucstat hyper inverse
    // concentration
    double GeneNucRatesHyperLogPrior() const {
        double total = 0;
        if (nucmode == 1) {
            total -= nucrelratehyperinvconc;
            total -= nucstathyperinvconc;
        }
        return total;
    }

    // omega
    
    double OmegaHyperLogPrior() const   {
        double total = 0;
        total -= omegameanhypermean;
        total -= omegameanhyperinvshape;
        total -= omegainvshapehypermean;
        total -= omegainvshapehyperinvshape;
        return total;
    }

    double OmegaLogPrior() const    {
        double total = 0;
        total += omegameanarray->GetLogProb();
        total += omegainvshapearray->GetLogProb();
        return total;
    }

    double GetLogLikelihood() const { return lnL; }

    //-------------------
    // Suff Stat Log Probs
    //-------------------

    // Branch lengths

    // suff stat for global branch lengths, as a function of lambda
    double LambdaHyperSuffStatLogProb() const {
        return hyperlengthsuffstat.GetLogProb(1.0, lambda);
    }

    // suff stat for gene-specific branch lengths, as a function of bl
    // hyperparameters
    double BranchLengthsHyperSuffStatLogProb() const {
        return lengthhypersuffstatarray->GetLogProb(*branchlength, blhyperinvshape);
    }

    // Nucleotide rates

    // suff stat for global nuc rates, as a function of nucleotide matrix
    // (which itself depends on nucstat and nucrelrate)
    double NucRatesSuffStatLogProb() const {
        return nucpathsuffstat.GetLogProb(*nucmatrix, *GetCodonStateSpace());
    }

    // suff stat for gene-specific nuc rates, as a function of nucrate
    // hyperparameters
    double NucRatesHyperSuffStatLogProb() const {
        double total = 0;
        total += nucrelratesuffstat.GetLogProb(nucrelratehypercenter, 1.0 / nucrelratehyperinvconc);
        total += nucstatsuffstat.GetLogProb(nucstathypercenter, 1.0 / nucstathyperinvconc);
        return total;
    }

    // omega

    double OmegaHyperSuffStatLogProb() const {
        double total = 0;

        double meanalpha = 1.0 / omegameanhyperinvshape;
        double meanbeta = meanalpha / omegameanhypermean;
        total += omegameanhypersuffstat.GetLogProb(meanalpha, meanbeta);

        double invshapealpha = 1.0 / omegainvshapehyperinvshape;
        double invshapebeta = invshapealpha / omegainvshapehypermean;
        total += omegainvshapehypersuffstat.GetLogProb(invshapealpha, invshapebeta);

        return total;
    }

    //-------------------
    // Log Probs for MH moves
    //-------------------

    // Branch lengths

    // logprob for moving lambda
    double LambdaHyperLogProb() const {
        return LambdaHyperLogPrior() + LambdaHyperSuffStatLogProb();
    }

    // logprob for moving hyperparameters of gene-specific branchlengths
    double BranchLengthsHyperLogProb() const {
        return BranchLengthsHyperInvShapeLogPrior() + BranchLengthsHyperSuffStatLogProb();
    }

    // Nucleotide rates

    // log prob for moving nuc rates hyper params
    double NucRatesHyperLogProb() const {
        return GeneNucRatesHyperLogPrior() + NucRatesHyperSuffStatLogProb();
    }

    // log prob for moving nuc rates
    double NucRatesLogProb() const { return GlobalNucRatesLogPrior() + NucRatesSuffStatLogProb(); }

    // Omega

    // log prob for moving omega hyperparameters
    double OmegaHyperLogProb() const { return OmegaHyperLogPrior() + OmegaHyperSuffStatLogProb(); }

    //-------------------
    // Moves
    //-------------------

    // all methods starting with Master are called only by master
    // for each such method, there is a corresponding method called by slave, and
    // starting with Slave
    //
    // all methods starting with Gene are called only be slaves, and do some work
    // across all genes allocated to that slave

    void MasterMove() override {
        int nrep = 10;

        for (int rep = 0; rep < nrep; rep++) {

            if (omegamode == 1) {
                MasterReceiveOmegaParameters();
                MoveOmegaHyperParameters();
                MasterSendOmegaHyperParameters();
            }

            // global branch lengths, or gene branch lengths hyperparameters
            if (blmode == 2) {
                MasterReceiveBranchLengthsSuffStat();
                ResampleBranchLengths();
                MoveLambda();
                MasterSendGlobalBranchLengths();
            } else if (blmode == 1) {
                MasterReceiveBranchLengthsHyperSuffStat();
                MoveBranchLengthsHyperParameters();
                MasterSendBranchLengthsHyperParameters();
            }

            // global nucrates, or gene nucrates hyperparameters
            if (nucmode == 2) {
                MasterReceiveNucPathSuffStat();
                MoveNucRates();
                MasterSendGlobalNucRates();
            } else if (nucmode == 1) {
                MasterReceiveNucRatesHyperSuffStat();
                MoveNucRatesHyperParameters();
                MasterSendNucRatesHyperParameters();
            }
        }

        // collect current state
        if (blmode != 2) {
            MasterReceiveGeneBranchLengths();
        }
        if (nucmode != 2) {
            MasterReceiveGeneNucRates();
        }
        MasterReceiveOmega();
        MasterReceiveLogProbs();
    }

    // slave move
    void SlaveMove() override {
        GeneResampleSub(1.0);

        int nrep = 10;

        for (int rep = 0; rep < nrep; rep++) {

            MoveGeneParameters(1.0);

            if (omegamode == 1) {
                SlaveSendOmegaParameters();
                SlaveReceiveOmegaHyperParameters();
            }

            // global branch lengths, or gene branch lengths hyperparameters
            if (blmode == 2) {
                SlaveSendBranchLengthsSuffStat();
                SlaveReceiveGlobalBranchLengths();
            } else if (blmode == 1) {
                SlaveSendBranchLengthsHyperSuffStat();
                SlaveReceiveBranchLengthsHyperParameters();
            }

            // global nucrates, or gene nucrates hyperparameters
            if (nucmode == 2) {
                SlaveSendNucPathSuffStat();
                SlaveReceiveGlobalNucRates();
            } else if (nucmode == 1) {
                SlaveSendNucRatesHyperSuffStat();
                SlaveReceiveNucRatesHyperParameters();
            }
        }

        // collect current state
        if (blmode != 2) {
            SlaveSendGeneBranchLengths();
        }
        if (nucmode != 2) {
            SlaveSendGeneNucRates();
        }
        SlaveSendOmega();
        SlaveSendLogProbs();
    }

    void GeneResampleSub(double frac) {
        for (int gene = 0; gene < GetLocalNgene(); gene++) {
            geneprocess[gene]->ResampleSub(frac);
        }
    }

    void MoveGeneParameters(int nrep)   {
        for (int gene = 0; gene < GetLocalNgene(); gene++) {
            geneprocess[gene]->MoveParameters(nrep);

            (*omegaarray)[gene] = geneprocess[gene]->GetMeanOmega();
            (*omegameanarray)[gene] = geneprocess[gene]->GetOmegaMean();
            (*omegainvshapearray)[gene] = geneprocess[gene]->GetOmegaInvShape();

            if (blmode != 2) {
                geneprocess[gene]->GetBranchLengths((*branchlengtharray)[gene]);
            }
            if (nucmode != 2) {
                geneprocess[gene]->GetNucRates((*nucrelratearray)[gene], (*nucstatarray)[gene]);
            }
        }
    }

    // Branch lengths

    void ResampleBranchLengths() {
        branchlength->GibbsResample(*lengthpathsuffstatarray);
    }

    void ResampleGeneBranchLengths()   {
        for (int gene = 0; gene < GetLocalNgene(); gene++) {
            geneprocess[gene]->ResampleBranchLengths();
            geneprocess[gene]->GetBranchLengths((*branchlengtharray)[gene]);
        }
    }

    void MoveLambda() {
        hyperlengthsuffstat.Clear();
        hyperlengthsuffstat.AddSuffStat(*branchlength);
        ScalingMove(lambda, 1.0, 10, &MultiGeneSiteOmegaModel::LambdaHyperLogProb,
                    &MultiGeneSiteOmegaModel::NoUpdate, this);
        ScalingMove(lambda, 0.3, 10, &MultiGeneSiteOmegaModel::LambdaHyperLogProb,
                    &MultiGeneSiteOmegaModel::NoUpdate, this);
        branchlength->SetScale(lambda);
    }

    void MoveBranchLengthsHyperParameters() {

        BranchLengthsHyperScalingMove(1.0, 10);
        BranchLengthsHyperScalingMove(0.3, 10);

        ScalingMove(blhyperinvshape, 1.0, 10, &MultiGeneSiteOmegaModel::BranchLengthsHyperLogProb,
                    &MultiGeneSiteOmegaModel::NoUpdate, this);
        ScalingMove(blhyperinvshape, 0.3, 10, &MultiGeneSiteOmegaModel::BranchLengthsHyperLogProb,
                    &MultiGeneSiteOmegaModel::NoUpdate, this);

        branchlengtharray->SetShape(1.0 / blhyperinvshape);
        MoveLambda();
    }

    double BranchLengthsHyperScalingMove(double tuning, int nrep) {
        double nacc = 0;
        double ntot = 0;
        for (int rep = 0; rep < nrep; rep++) {
            for (int j = 0; j < Nbranch; j++) {
                double deltalogprob =
                    -branchlength->GetLogProb(j) -
                    lengthhypersuffstatarray->GetVal(j).GetLogProb(
                        1.0 / blhyperinvshape, 1.0 / blhyperinvshape / branchlength->GetVal(j));
                double m = tuning * (Random::Uniform() - 0.5);
                double e = exp(m);
                (*branchlength)[j] *= e;
                deltalogprob +=
                    branchlength->GetLogProb(j) +
                    lengthhypersuffstatarray->GetVal(j).GetLogProb(
                        1.0 / blhyperinvshape, 1.0 / blhyperinvshape / branchlength->GetVal(j));
                deltalogprob += m;
                int accepted = (log(Random::Uniform()) < deltalogprob);
                if (accepted) {
                    nacc++;
                } else {
                    (*branchlength)[j] /= e;
                }
                ntot++;
            }
        }
        return nacc / ntot;
    }

    // Nucleotide rates

    void MoveNucRatesHyperParameters() {
        ProfileMove(nucrelratehypercenter, 1.0, 1, 10, &MultiGeneSiteOmegaModel::NucRatesHyperLogProb,
                    &MultiGeneSiteOmegaModel::NoUpdate, this);
        ProfileMove(nucrelratehypercenter, 0.3, 1, 10, &MultiGeneSiteOmegaModel::NucRatesHyperLogProb,
                    &MultiGeneSiteOmegaModel::NoUpdate, this);
        ProfileMove(nucrelratehypercenter, 0.1, 3, 10, &MultiGeneSiteOmegaModel::NucRatesHyperLogProb,
                    &MultiGeneSiteOmegaModel::NoUpdate, this);
        ScalingMove(nucrelratehyperinvconc, 1.0, 10, &MultiGeneSiteOmegaModel::NucRatesHyperLogProb,
                    &MultiGeneSiteOmegaModel::NoUpdate, this);
        ScalingMove(nucrelratehyperinvconc, 0.3, 10, &MultiGeneSiteOmegaModel::NucRatesHyperLogProb,
                    &MultiGeneSiteOmegaModel::NoUpdate, this);
        ScalingMove(nucrelratehyperinvconc, 0.03, 10, &MultiGeneSiteOmegaModel::NucRatesHyperLogProb,
                    &MultiGeneSiteOmegaModel::NoUpdate, this);

        ProfileMove(nucstathypercenter, 1.0, 1, 10, &MultiGeneSiteOmegaModel::NucRatesHyperLogProb,
                    &MultiGeneSiteOmegaModel::NoUpdate, this);
        ProfileMove(nucstathypercenter, 0.3, 1, 10, &MultiGeneSiteOmegaModel::NucRatesHyperLogProb,
                    &MultiGeneSiteOmegaModel::NoUpdate, this);
        ProfileMove(nucstathypercenter, 0.1, 2, 10, &MultiGeneSiteOmegaModel::NucRatesHyperLogProb,
                    &MultiGeneSiteOmegaModel::NoUpdate, this);
        ScalingMove(nucstathyperinvconc, 1.0, 10, &MultiGeneSiteOmegaModel::NucRatesHyperLogProb,
                    &MultiGeneSiteOmegaModel::NoUpdate, this);
        ScalingMove(nucstathyperinvconc, 0.3, 10, &MultiGeneSiteOmegaModel::NucRatesHyperLogProb,
                    &MultiGeneSiteOmegaModel::NoUpdate, this);
        ScalingMove(nucstathyperinvconc, 0.03, 10, &MultiGeneSiteOmegaModel::NucRatesHyperLogProb,
                    &MultiGeneSiteOmegaModel::NoUpdate, this);

        nucrelratearray->SetConcentration(1.0 / nucrelratehyperinvconc);
        nucstatarray->SetConcentration(1.0 / nucstathyperinvconc);
    }

    void MoveNucRates() {
        vector<double> &nucrelrate = (*nucrelratearray)[0];
        ProfileMove(nucrelrate, 0.1, 1, 10, &MultiGeneSiteOmegaModel::NucRatesLogProb,
                    &MultiGeneSiteOmegaModel::UpdateNucMatrix, this);
        ProfileMove(nucrelrate, 0.03, 3, 10, &MultiGeneSiteOmegaModel::NucRatesLogProb,
                    &MultiGeneSiteOmegaModel::UpdateNucMatrix, this);
        ProfileMove(nucrelrate, 0.01, 3, 10, &MultiGeneSiteOmegaModel::NucRatesLogProb,
                    &MultiGeneSiteOmegaModel::UpdateNucMatrix, this);

        vector<double> &nucstat = (*nucstatarray)[0];
        ProfileMove(nucstat, 0.1, 1, 10, &MultiGeneSiteOmegaModel::NucRatesLogProb,
                    &MultiGeneSiteOmegaModel::UpdateNucMatrix, this);
        ProfileMove(nucstat, 0.01, 1, 10, &MultiGeneSiteOmegaModel::NucRatesLogProb,
                    &MultiGeneSiteOmegaModel::UpdateNucMatrix, this);
    }

    // Omega

    void MoveOmegaHyperParameters() {
        omegameanhypersuffstat.Clear();
        omegameanhypersuffstat.AddSuffStat(*omegameanarray);

        omegainvshapehypersuffstat.Clear();
        omegainvshapehypersuffstat.AddSuffStat(*omegainvshapearray);

        ScalingMove(omegameanhypermean, 1.0, 10, &MultiGeneSiteOmegaModel::OmegaHyperLogProb,
                    &MultiGeneSiteOmegaModel::NoUpdate, this);
        ScalingMove(omegameanhypermean, 0.3, 10, &MultiGeneSiteOmegaModel::OmegaHyperLogProb,
                    &MultiGeneSiteOmegaModel::NoUpdate, this);
        ScalingMove(omegameanhyperinvshape, 1.0, 10, &MultiGeneSiteOmegaModel::OmegaHyperLogProb,
                    &MultiGeneSiteOmegaModel::NoUpdate, this);
        ScalingMove(omegameanhyperinvshape, 0.3, 10, &MultiGeneSiteOmegaModel::OmegaHyperLogProb,
                    &MultiGeneSiteOmegaModel::NoUpdate, this);

        ScalingMove(omegainvshapehypermean, 1.0, 10, &MultiGeneSiteOmegaModel::OmegaHyperLogProb,
                    &MultiGeneSiteOmegaModel::NoUpdate, this);
        ScalingMove(omegainvshapehypermean, 0.3, 10, &MultiGeneSiteOmegaModel::OmegaHyperLogProb,
                    &MultiGeneSiteOmegaModel::NoUpdate, this);
        ScalingMove(omegainvshapehyperinvshape, 1.0, 10, &MultiGeneSiteOmegaModel::OmegaHyperLogProb,
                    &MultiGeneSiteOmegaModel::NoUpdate, this);
        ScalingMove(omegainvshapehyperinvshape, 0.3, 10, &MultiGeneSiteOmegaModel::OmegaHyperLogProb,
                    &MultiGeneSiteOmegaModel::NoUpdate, this);

        double meanalpha = 1.0 / omegameanhyperinvshape;
        double meanbeta = meanalpha / omegameanhypermean;
        omegameanarray->SetShape(meanalpha);
        omegameanarray->SetScale(meanbeta);

        double invshapealpha = 1.0 / omegainvshapehyperinvshape;
        double invshapebeta = invshapealpha / omegainvshapehypermean;
        omegainvshapearray->SetShape(invshapealpha);
        omegainvshapearray->SetScale(invshapebeta);
    }

    //-------------------
    // MPI send / receive
    //-------------------

    // Branch lengths

    void MasterSendGlobalBranchLengths() { MasterSendGlobal(*branchlength); }

    void SlaveReceiveGlobalBranchLengths() {
        SlaveReceiveGlobal(*branchlength);
        for (int gene = 0; gene < GetLocalNgene(); gene++) {
            geneprocess[gene]->SetBranchLengths(*branchlength);
        }
    }

    void MasterSendBranchLengthsHyperParameters() {
        MasterSendGlobal(*branchlength, blhyperinvshape);
    }

    void SlaveReceiveBranchLengthsHyperParameters() {
        SlaveReceiveGlobal(*branchlength, blhyperinvshape);
        for (int gene = 0; gene < GetLocalNgene(); gene++) {
            geneprocess[gene]->SetBranchLengthsHyperParameters(*branchlength, blhyperinvshape);
        }
    }

    void MasterSendGeneBranchLengths() {
        MasterSendGeneArray(*branchlengtharray);
    }

    void SlaveReceiveGeneBranchLengths() {
        SlaveReceiveGeneArray(*branchlengtharray);
        for (int gene = 0; gene < GetLocalNgene(); gene++) {
            geneprocess[gene]->SetBranchLengths(branchlengtharray->GetVal(gene));
        }
    }

    void SlaveSendGeneBranchLengths() {
        SlaveSendGeneArray(*branchlengtharray);
    }

    void MasterReceiveGeneBranchLengths() {
        MasterReceiveGeneArray(*branchlengtharray);
    }

    void SlaveSendBranchLengthsSuffStat() {
        lengthpathsuffstatarray->Clear();
        for (int gene = 0; gene < GetLocalNgene(); gene++) {
            geneprocess[gene]->CollectLengthSuffStat();
            lengthpathsuffstatarray->Add(*geneprocess[gene]->GetLengthPathSuffStatArray());
        }
        SlaveSendAdditive(*lengthpathsuffstatarray);
    }

    void MasterReceiveBranchLengthsSuffStat() {
        lengthpathsuffstatarray->Clear();
        MasterReceiveAdditive(*lengthpathsuffstatarray);
    }

    void SlaveSendBranchLengthsHyperSuffStat() {
        lengthhypersuffstatarray->Clear();
        lengthhypersuffstatarray->AddSuffStat(*branchlengtharray);
        SlaveSendAdditive(*lengthhypersuffstatarray);
    }

    void MasterReceiveBranchLengthsHyperSuffStat() {
        lengthhypersuffstatarray->Clear();
        MasterReceiveAdditive(*lengthhypersuffstatarray);
    }

    // Nucleotide Rates

    void MasterSendGlobalNucRates() {
        MasterSendGlobal(nucrelratearray->GetVal(0), nucstatarray->GetVal(0));
    }

    void SlaveReceiveGlobalNucRates() {
        SlaveReceiveGlobal((*nucrelratearray)[0], (*nucstatarray)[0]);

        for (int gene = 0; gene < GetLocalNgene(); gene++) {
            geneprocess[gene]->SetNucRates((*nucrelratearray)[0], (*nucstatarray)[0]);
        }
    }

    void MasterSendGeneNucRates() {
        MasterSendGeneArray(*nucrelratearray, *nucstatarray);
    }

    void SlaveReceiveGeneNucRates() {
        SlaveReceiveGeneArray(*nucrelratearray, *nucstatarray);

        for (int gene = 0; gene < GetLocalNgene(); gene++) {
            geneprocess[gene]->SetNucRates((*nucrelratearray)[gene], (*nucstatarray)[gene]);
        }
    }

    void SlaveSendGeneNucRates() {
        SlaveSendGeneArray(*nucrelratearray, *nucstatarray);
    }

    void MasterReceiveGeneNucRates() {
        MasterReceiveGeneArray(*nucrelratearray, *nucstatarray);
    }

    void MasterSendNucRatesHyperParameters() {
        MasterSendGlobal(nucrelratehypercenter, nucrelratehyperinvconc);
        MasterSendGlobal(nucstathypercenter, nucstathyperinvconc);
    }

    void SlaveReceiveNucRatesHyperParameters() {
        SlaveReceiveGlobal(nucrelratehypercenter, nucrelratehyperinvconc);
        SlaveReceiveGlobal(nucstathypercenter, nucstathyperinvconc);

        for (int gene = 0; gene < GetLocalNgene(); gene++) {
            geneprocess[gene]->SetNucRatesHyperParameters(nucrelratehypercenter, nucrelratehyperinvconc,
                                                          nucstathypercenter, nucstathyperinvconc);
        }
    }

    void SlaveSendNucRatesHyperSuffStat() {
        nucrelratesuffstat.Clear();
        nucrelratearray->AddSuffStat(nucrelratesuffstat);
        SlaveSendAdditive(nucrelratesuffstat);

        nucstatsuffstat.Clear();
        nucstatarray->AddSuffStat(nucstatsuffstat);
        SlaveSendAdditive(nucstatsuffstat);
    }

    void MasterReceiveNucRatesHyperSuffStat() {
        nucrelratesuffstat.Clear();
        MasterReceiveAdditive(nucrelratesuffstat);

        nucstatsuffstat.Clear();
        MasterReceiveAdditive(nucstatsuffstat);
    }

    void SlaveSendNucPathSuffStat() {
        nucpathsuffstat.Clear();
        for (int gene = 0; gene < GetLocalNgene(); gene++) {
            geneprocess[gene]->CollectNucPathSuffStat();
            nucpathsuffstat += geneprocess[gene]->GetNucPathSuffStat();
        }

        SlaveSendAdditive(nucpathsuffstat);
    }

    void MasterReceiveNucPathSuffStat() {
        nucpathsuffstat.Clear();
        MasterReceiveAdditive(nucpathsuffstat);
    }

    // omega (and hyperparameters)

    void SlaveSendOmega() { 
        SlaveSendGeneArray(*omegaarray); 
    }

    void MasterReceiveOmega() {
        MasterReceiveGeneArray(*omegaarray); 
    }

    void SlaveSendOmegaParameters() {
        SlaveSendGeneArray(*omegameanarray); 
        SlaveSendGeneArray(*omegainvshapearray); 
    }

    void MasterReceiveOmegaParameters() {
        MasterReceiveGeneArray(*omegameanarray); 
        MasterReceiveGeneArray(*omegainvshapearray); 
    }

    void MasterSendOmegaParameters() { 
        MasterSendGeneArray(*omegameanarray); 
        MasterSendGeneArray(*omegainvshapearray); 
    }

    void SlaveReceiveOmegaParameters() {
        SlaveReceiveGeneArray(*omegameanarray);
        SlaveReceiveGeneArray(*omegainvshapearray);
        for (int gene = 0; gene < GetLocalNgene(); gene++) {
            geneprocess[gene]->SetOmegaParameters((*omegameanarray)[gene], (*omegainvshapearray)[gene]);
        }
    }

    // omega hyperparameters

    void MasterSendOmegaHyperParameters() { 
        MasterSendGlobal(omegameanhypermean, omegameanhyperinvshape); 
        MasterSendGlobal(omegainvshapehypermean, omegainvshapehyperinvshape); 
    }

    void SlaveReceiveOmegaHyperParameters() {
        SlaveReceiveGlobal(omegameanhypermean, omegameanhyperinvshape);
        SlaveReceiveGlobal(omegainvshapehypermean, omegainvshapehyperinvshape);
        for (int gene = 0; gene < GetLocalNgene(); gene++) {
            geneprocess[gene]->SetOmegaHyperParameters(omegameanhypermean, omegameanhyperinvshape, omegainvshapehypermean, omegainvshapehyperinvshape);
        }
    }

    // log probs

    void SlaveSendLogProbs() {
        GeneLogPrior = 0;
        lnL = 0;
        for (int gene = 0; gene < GetLocalNgene(); gene++) {
            GeneLogPrior += geneprocess[gene]->GetLogPrior();
            lnL += geneprocess[gene]->GetLogLikelihood();
        }
        SlaveSendAdditive(GeneLogPrior);
        SlaveSendAdditive(lnL);
    }

    void MasterReceiveLogProbs() {
        GeneLogPrior = 0;
        MasterReceiveAdditive(GeneLogPrior);
        lnL = 0;
        MasterReceiveAdditive(lnL);
    }
};
