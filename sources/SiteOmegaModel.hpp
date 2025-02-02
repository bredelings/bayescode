#include "CodonSequenceAlignment.hpp"
#include "CodonSubMatrix.hpp"
#include "CodonSuffStat.hpp"
#include "GTRSubMatrix.hpp"
#include "GammaSuffStat.hpp"
#include "IIDGamma.hpp"
#include "PhyloProcess.hpp"
#include "ProbModel.hpp"
#include "Tree.hpp"

/**
 * \brief A Muse and Gaut codon model with site-specific omega's
 *
 */

class SiteOmegaModel : public ProbModel {

    // tree and data
    const Tree *tree;
    FileSequenceAlignment *data;
    const TaxonSet *taxonset;
    const CodonSequenceAlignment *codondata;

    int Nsite;
    int Ntaxa;
    int Nbranch;

    int blmode;
    int nucmode;

    // Branch lengths

    double lambda;
    BranchIIDGamma *blhypermean;
    double blhyperinvshape;
    GammaWhiteNoise *branchlength;

    // Poisson suffstats for substitution histories, as a function of branch
    // lengths
    PoissonSuffStatBranchArray *lengthpathsuffstatarray;

    // suff stats branch lengths, as a function of their hyper parameter lambda
    // (bl are iid gamma, of scale parameter lambda)
    GammaSuffStat hyperlengthsuffstat;

    // Nucleotide rates

    vector<double> nucrelratehypercenter;
    double nucrelratehyperinvconc;
    vector<double> nucstathypercenter;
    double nucstathyperinvconc;

    vector<double> nucrelrate;
    vector<double> nucstat;
    GTRSubMatrix *nucmatrix;

    // path suff stat can be summarized in terms of 4x4 suff stats, as a function
    // of nucleotide rates
    NucPathSuffStat nucpathsuffstat;

    // Omega

    double omegameanhypermean;
    double omegameanhyperinvshape;
    double omegainvshapehypermean;
    double omegainvshapehyperinvshape;

    // omega across sites: Gamma distribution
    // of mean omegamean and inverse shape parameter omegainvshape
    double omegamean;
    double omegainvshape;

    IIDGamma *omegaarray;

    // a codon matrix array (parameterized by nucmatrix and omega)
    MGOmegaCodonSubMatrixArray *codonmatrixarray;

    // PhyloProcess

    PhyloProcess *phyloprocess;

    // generic suff stats for substitution paths
    PathSuffStatArray *pathsuffstatarray;

    // or, alternatively, collected as a simple Poisson suff stat, as a function
    // of omega
    OmegaPathSuffStatArray *omegapathsuffstatarray;

    // suffstat for resampling mean and invshape of distribution of omega's across
    // sites
    GammaSuffStat omegahypersuffstat;

  public:

    //-------------------
    // Construction and allocation
    // ------------------

    //! \brief constructor, parameterized by names of data and tree files
    //!
    //! Note: in itself, the constructor does not allocate the model;
    //! It only reads the data and tree file and register them together.
    SiteOmegaModel(string datafile, string treefile) {

        blmode = 0;
        nucmode = 0;

        data = new FileSequenceAlignment(datafile);
        codondata = new CodonSequenceAlignment(data, true);

        Nsite = codondata->GetNsite();  // # columns
        Ntaxa = codondata->GetNtaxa();

        taxonset = codondata->GetTaxonSet();

        // get tree from file (newick format)
        Tree* tmptree = new Tree(treefile);
        // check whether tree and data fits together
        tmptree->RegisterWith(taxonset);
        tmptree->SetIndices();
        tree = tmptree;

        Nbranch = tree->GetNbranch();
    }

    SiteOmegaModel(const CodonSequenceAlignment* incodondata, const Tree* intree)   {

        blmode = 0;
        nucmode = 0;

        codondata = incodondata;

        Nsite = codondata->GetNsite();  // # columns
        Ntaxa = codondata->GetNtaxa();

        taxonset = codondata->GetTaxonSet();

        tree = intree;
        Nbranch = tree->GetNbranch();
    }

    //! model allocation
    void Allocate() {

        // Branch lengths

        lambda = 10.0;
        blhypermean = new BranchIIDGamma(*tree, 1.0, lambda);
        blhypermean->SetAllBranches(1.0 / lambda);
        blhyperinvshape = 1.0;
        branchlength = new GammaWhiteNoise(*tree, *blhypermean, 1.0 / blhyperinvshape);
        lengthpathsuffstatarray = new PoissonSuffStatBranchArray(*tree);

        // Nucleotide rates

        nucrelratehypercenter.assign(Nrr, 1.0 / Nrr);
        nucrelratehyperinvconc = 1.0 / Nrr;

        nucstathypercenter.assign(Nnuc, 1.0 / Nnuc);
        nucstathyperinvconc = 1.0 / Nnuc;

        nucrelrate.assign(Nrr, 0);
        Random::DirichletSample(nucrelrate, nucrelratehypercenter, 1.0 / nucrelratehyperinvconc);

        nucstat.assign(Nnuc, 0);
        Random::DirichletSample(nucstat, nucstathypercenter, 1.0 / nucstathyperinvconc);

        nucmatrix = new GTRSubMatrix(Nnuc, nucrelrate, nucstat, true);

        // Omega

        omegamean = 1.0;
        omegainvshape = 0.3;
        double alpha = 1.0 / omegainvshape;
        double beta = alpha / omegamean;
        omegaarray = new IIDGamma(Nsite, alpha, beta);
        omegapathsuffstatarray = new OmegaPathSuffStatArray(Nsite);

        codonmatrixarray =
            new MGOmegaCodonSubMatrixArray(GetCodonStateSpace(), nucmatrix, omegaarray);

        phyloprocess = new PhyloProcess(tree, codondata, branchlength, 0, codonmatrixarray);
        pathsuffstatarray = new PathSuffStatArray(Nsite);

        phyloprocess->Unfold();
    }

    //-------------------
    // Accessors
    // ------------------

    //! const access to codon state space
    CodonStateSpace *GetCodonStateSpace() const {
        return (CodonStateSpace *)codondata->GetStateSpace();
    }

    int GetNsite() const { return Nsite; }
    
    //-------------------
    // Setting and updating
    // ------------------

    //! \brief set estimation method for branch lengths and nuc rates
    //!
    //! Used in a multigene context.
    //! - mode == 2: global
    //! - mode == 1: gene specific, with hyperparameters estimated across genes
    //! - mode == 0: gene-specific, with fixed hyperparameters
    void SetAcrossGenesModes(int inblmode, int innucmode) {
        blmode = inblmode;
        nucmode = innucmode;
    }

    // Branch lengths

    //! whether branch lengths are fixed externally (e.g. when branch lengths are
    //! shared across genes in a multi-gene context)
    bool FixedBranchLengths() const { return blmode == 2; }

    //! set branch lengths to a new value (multi-gene analyses)
    void SetBranchLengths(const BranchSelector<double> &inbranchlength) {
        branchlength->Copy(inbranchlength);
    }

    //! get a copy of branch lengths into array given as argument
    void GetBranchLengths(BranchArray<double> &inbranchlength) const {
        inbranchlength.Copy(*branchlength);
    }

    //! set branch lengths hyperparameters to a new value (multi-gene analyses)
    void SetBranchLengthsHyperParameters(const BranchSelector<double> &inblmean,
                                                        double inblinvshape) {
        blhypermean->Copy(inblmean);
        blhyperinvshape = inblinvshape;
        branchlength->SetShape(1.0 / blhyperinvshape);
    }

    // Nucleotide rates

    //! whether nuc rates are fixed externally (e.g. when nuc rates are shared
    //! across genes in a multi-gene context)
    bool FixedNucRates() const { return nucmode == 2; }

    //! set nucleotide rates (relative exchangeabilities and eq. frequencies) to a
    //! new value (multi-gene analyses)
    void SetNucRates(const std::vector<double> &innucrelrate,
                                    const std::vector<double> &innucstat) {
        nucrelrate = innucrelrate;
        nucstat = innucstat;
        TouchMatrices();
    }

    //! get a copy of nucleotide rates into arrays given as arguments
    void GetNucRates(std::vector<double> &innucrelrate,
                                    std::vector<double> &innucstat) const {
        innucrelrate = nucrelrate;
        innucstat = nucstat;
    }

    //! set nucleotide rates hyperparameters to a new value (multi-gene analyses)
    void SetNucRatesHyperParameters(const std::vector<double> &innucrelratehypercenter,
                                                   double innucrelratehyperinvconc,
                                                   const std::vector<double> &innucstathypercenter,
                                                   double innucstathyperinvconc) {
        nucrelratehypercenter = innucrelratehypercenter;
        nucrelratehyperinvconc = innucrelratehyperinvconc;
        nucstathypercenter = innucstathypercenter;
        nucstathyperinvconc = innucstathyperinvconc;
    }

    // Omega

    void SetOmegaParameters(double inomegamean, double inomegainvshape) {
        omegamean = inomegamean;
        omegainvshape = inomegainvshape;
        double alpha = 1.0 / omegainvshape;
        double beta = alpha / omegamean;
        omegaarray->SetShape(alpha);
        omegaarray->SetScale(beta);
    }

    void SetOmegaHyperParameters(double inomegameanhypermean, double inomegameanhyperinvshape, double inomegainvshapehypermean, double inomegainvshapehyperinvshape)    {
        omegameanhypermean = inomegameanhypermean;
        omegameanhyperinvshape = inomegameanhyperinvshape;
        omegainvshapehypermean = inomegainvshapehypermean;
        omegainvshapehyperinvshape = inomegainvshapehyperinvshape;
    }

    //! \brief tell the nucleotide matrix that its parameters have changed and
    //! that it should be updated
    //!
    //! The matrix is not directly updated at that step. Instead, corruption is
    //! notified, such that the matrix knows that it will have to recalculate
    //! whichever component is requested later on upon demand.
    void TouchNucMatrix() {
        nucmatrix->CopyStationary(nucstat);
        nucmatrix->CorruptMatrix();
    }

    //! \brief tell the codon matrix that its parameters have changed and that it
    //! should be updated
    //!
    //! The matrix is not directly updated at that step. Instead, corruption is
    //! notified, such that the matrix knows that it will have to recalculate
    //! whichever component is requested later on upon demand.
    void TouchCodonMatrices() {
        codonmatrixarray->UpdateCodonMatrices();
    }

    //! \brief tell the nucleotide and the codon matrices that their parameters
    //! have changed and that they should be updated
    //!
    //! Just successive calls to TouchNucMatrix() and then TouchCodonMatrix();
    void TouchMatrices() {
        TouchNucMatrix();
        TouchCodonMatrices();
    }

    //! \brief dummy function that does not do anything.
    //!
    //! Used for the templates of ScalingMove, SlidingMove and ProfileMove
    //! (defined in ProbModel), all of which require a void (*f)(void) function
    //! pointer to be called after changing the value of the focal parameter.
    void NoUpdate() {}

    //! \brief global update function (includes the stochastic mapping of
    //! character history)
    void Update() override {
        if (blmode == 0) {
            blhypermean->SetAllBranches(1.0 / lambda);
        }
        SetOmegaParameters(omegamean,omegainvshape);
        TouchMatrices();
        ResampleSub(1.0);
    }

    //-------------------
    // Posterior Predictive
    // ------------------

    //! \brief post pred function (does the update of all fields before doing the
    //! simulation)
    void PostPred(string name) override {
        if (blmode == 0) {
            blhypermean->SetAllBranches(1.0 / lambda);
        }
        SetOmegaParameters(omegamean,omegainvshape);
        TouchMatrices();
        phyloprocess->PostPredSample(name);
    }

    //-------------------
    // Priors and likelihood
    //-------------------

    //! \brief return total log prior
    //!
    //! Note: up to some multiplicative constant
    double GetLogPrior() const {
        double total = 0;

        if (!FixedBranchLengths()) {
            total += BranchLengthsLogPrior();
        }
        if (!FixedNucRates()) {
            total += NucRatesLogPrior();
        }
        total += OmegaHyperLogPrior();
        total += OmegaLogPrior();
        return total;
    }

    //! return current value of likelihood (pruning-style, i.e. integrated over
    //! all substitution histories)
    double GetLogLikelihood() const { return phyloprocess->GetLogLikelihood(); }

    //! return joint log prob (log prior + log likelihood)
    double GetLogProb() const override { return GetLogPrior() + GetLogLikelihood(); }

    // Branch lengths

    //! log prior over branch lengths (iid exponential of rate lambda)
    double BranchLengthsLogPrior() const {
        double total = 0;
        if (blmode == 0) {
            total += LambdaHyperLogPrior();
        }
        total += branchlength->GetLogProb();
        return total;
    }

    //! \brief log prior over hyperparameter of prior over branch lengths (here,
    //! lambda ~ exponential of rate 10)
    double LambdaHyperLogPrior() const { return -lambda / 10; }

    // Nucleotide rates

    //! log prior over nucleotide relative exchangeabilities (nucrelrate) and eq.
    //! freqs. (nucstat) -- uniform Dirichlet in both cases
    double NucRatesLogPrior() const {
        double total = 0;
        total += Random::logDirichletDensity(nucrelrate, nucrelratehypercenter,
                                             1.0 / nucrelratehyperinvconc);
        total += Random::logDirichletDensity(nucstat, nucstathypercenter, 1.0 / nucstathyperinvconc);
        return total;
    }

    // Omega

    double OmegaHyperLogPrior() const {
        double total = 0;

        double meanalpha = 1.0 / omegameanhyperinvshape;
        double meanbeta = meanalpha / omegameanhypermean;
        total += Random::logGammaDensity(omegamean, meanalpha, meanbeta);

        double invshapealpha = 1.0 / omegainvshapehyperinvshape;
        double invshapebeta = invshapealpha / omegainvshapehypermean;
        total += Random::logGammaDensity(omegainvshape, invshapealpha, invshapebeta);

        return total;
    }

    double OmegaLogPrior() const { return omegaarray->GetLogProb(); }

    //-------------------
    // Suff Stat and suffstatlogprobs
    //-------------------

    // Branch lengths

    //! \brief const access to array of length-pathsuffstats across branches
    //!
    //! Useful for resampling branch lengths conditional on the current
    //! substitution mapping
    const PoissonSuffStatBranchArray *GetLengthPathSuffStatArray() const {
        return lengthpathsuffstatarray;
    }

    //! \brief return log prob of current substitution mapping, as a function of
    //! branch lengths
    //!
    //! Calculated using the lengthpathsuffstat
    //! (which summarizes all information about how the prob of the substitution
    //! mapping depends on branch lengths). lengthpathsuffstat is assumed to be
    //! updated.
    double LambdaHyperSuffStatLogProb() const {
        return hyperlengthsuffstat.GetLogProb(1.0, lambda);
    }

    //! collect sufficient statistics for moving branch lengths (directly from the
    //! substitution mappings)
    void CollectLengthSuffStat() {
        lengthpathsuffstatarray->Clear();
        lengthpathsuffstatarray->AddLengthPathSuffStat(*phyloprocess);
    }

    // Nucleotide rates

    //! \brief const acess to nuc-pathsuffstat
    //!
    //! Useful for resampling nucleotide relative exchangeabilities (nucrelrate)
    //! and equilibrium frequencies (nucstat) conditional on the current
    //! substitution mapping.
    const NucPathSuffStat &GetNucPathSuffStat() const { return nucpathsuffstat; }

    //! \brief return log prob of current substitution mapping, as a function of
    //! nucleotide parameters (nucrelrate and nucstat)
    //!
    //! Calculated using nucpathsuffstat
    //! (which summarizes all information about how the probability of the
    //! substitution mapping depends on nucleotide mutation rates) and the
    //! nucmatrix. Both nucpathsuffstat and nucmatrix are assumed to be updated.
    double NucRatesSuffStatLogProb() const {
        return nucpathsuffstat.GetLogProb(*nucmatrix, *GetCodonStateSpace());
    }

    //! collect sufficient statistics for moving nucleotide rates (based on
    //! generic sufficient statistics stored in pathsuffstat)
    void CollectNucPathSuffStat() {
        TouchMatrices();
        nucpathsuffstat.Clear();
        nucpathsuffstat.AddSuffStat(*codonmatrixarray, *pathsuffstatarray);
    }

    // Omega
    
    double OmegaHyperSuffStatLogProb() const {
        double alpha = 1.0 / omegainvshape;
        double beta = alpha / omegamean;
        return omegahypersuffstat.GetLogProb(alpha, beta);
    }


    // Paths 

    //! collect generic sufficient statistics from substitution mappings
    void CollectPathSuffStat() {
        pathsuffstatarray->Clear();
        pathsuffstatarray->AddSuffStat(*phyloprocess);
    }

    //! \brief return log prob of the current substitution mapping, as a function
    //! of the current codon substitution process
    //!
    //! Calculated using pathsuffstat (which summarizes all information about the
    //! substitution mapping) and the codonmatrix. Both pathsuffstat and
    //! codonmatrix are assumed to be updated.
    double PathSuffStatLogProb() const { return pathsuffstatarray->GetLogProb(*codonmatrixarray); }

    //-------------------
    //  Log probs for MH moves
    //-------------------

    // Branch lengths

    //! \brief log prob factor to be recomputed when moving branch lengths
    //! hyperparameters (here, lambda)
    double LambdaHyperLogProb() const {
        return LambdaHyperLogPrior() + LambdaHyperSuffStatLogProb();
    }

    // Nucleotide rates

    //! \brief log prob factor to be recomputed when moving nucleotide mutation
    //! rate parameters (nucrelrate and nucstat)
    double NucRatesLogProb() const { return NucRatesLogPrior() + NucRatesSuffStatLogProb(); }

    // for moving omegamean and omegainvshape
    double OmegaHyperLogProb() const { return OmegaHyperLogPrior() + OmegaHyperSuffStatLogProb(); }

    //-------------------
    //  Moves
    //-------------------

    //! \brief complete MCMC move schedule
    double Move() override {
        ResampleSub(1.0);
        MoveParameters(30);
        return 1.0;
    }

    //! Gibbs resample substitution mappings conditional on current parameter
    //! configuration
    void ResampleSub(double frac) {
        TouchMatrices();
        phyloprocess->Move(frac);
    }

    //! complete series of MCMC moves on all parameters (repeated nrep times)
    void MoveParameters(int nrep) {
        for (int rep = 0; rep < nrep; rep++) {
            if (!FixedBranchLengths()) {
                MoveBranchLengths();
            }

            CollectPathSuffStat();

            MoveOmega();
            MoveOmegaHyperParameters();

            if (!FixedNucRates()) {
                TouchMatrices();
                MoveNucRates();
            }
        }
    }

    // Branch lengths

    //! overall schedule branch length updatdes
    void MoveBranchLengths() {
        ResampleBranchLengths();
        if (blmode == 0) {
            MoveLambda();
        }
    }

    //! Gibbs resample branch lengths (based on sufficient statistics and current
    //! value of lambda)
    void ResampleBranchLengths() {
        CollectLengthSuffStat();
        branchlength->GibbsResample(*lengthpathsuffstatarray);
    }

    //! MH move on branch lengths hyperparameters (here, scaling move on lambda,
    //! based on suffstats for branch lengths)
    void MoveLambda() {
        hyperlengthsuffstat.Clear();
        hyperlengthsuffstat.AddSuffStat(*branchlength);
        ScalingMove(lambda, 1.0, 10, &SiteOmegaModel::LambdaHyperLogProb, &SiteOmegaModel::NoUpdate,
                    this);
        ScalingMove(lambda, 0.3, 10, &SiteOmegaModel::LambdaHyperLogProb, &SiteOmegaModel::NoUpdate,
                    this);
        blhypermean->SetAllBranches(1.0 / lambda);
    }

    // Nucleotide rates

    //! MH moves on nucleotide rate parameters (nucrelrate and nucstat: using
    //! ProfileMove)
    void MoveNucRates() {
        CollectNucPathSuffStat();

        ProfileMove(nucrelrate, 0.1, 1, 3, &SiteOmegaModel::NucRatesLogProb,
                    &SiteOmegaModel::TouchNucMatrix, this);
        ProfileMove(nucrelrate, 0.03, 3, 3, &SiteOmegaModel::NucRatesLogProb,
                    &SiteOmegaModel::TouchNucMatrix, this);
        ProfileMove(nucrelrate, 0.01, 3, 3, &SiteOmegaModel::NucRatesLogProb,
                    &SiteOmegaModel::TouchNucMatrix, this);

        ProfileMove(nucstat, 0.1, 1, 3, &SiteOmegaModel::NucRatesLogProb,
                    &SiteOmegaModel::TouchNucMatrix, this);
        ProfileMove(nucstat, 0.01, 1, 3, &SiteOmegaModel::NucRatesLogProb,
                    &SiteOmegaModel::TouchNucMatrix, this);

        TouchMatrices();
    }

    // Omega

    void MoveOmega() {
        omegapathsuffstatarray->Clear();
        omegapathsuffstatarray->AddSuffStat(*codonmatrixarray, *pathsuffstatarray);
        omegaarray->GibbsResample(*omegapathsuffstatarray);
        TouchCodonMatrices();
    }

    void MoveOmegaHyperParameters() {
        omegahypersuffstat.Clear();
        omegahypersuffstat.AddSuffStat(*omegaarray);
        ScalingMove(omegamean, 1.0, 10, &SiteOmegaModel::OmegaHyperLogProb,
                    &SiteOmegaModel::NoUpdate, this);
        ScalingMove(omegamean, 0.3, 10, &SiteOmegaModel::OmegaHyperLogProb,
                    &SiteOmegaModel::NoUpdate, this);
        ScalingMove(omegainvshape, 1.0, 10, &SiteOmegaModel::OmegaHyperLogProb,
                    &SiteOmegaModel::NoUpdate, this);
        ScalingMove(omegainvshape, 0.3, 10, &SiteOmegaModel::OmegaHyperLogProb,
                    &SiteOmegaModel::NoUpdate, this);
        double alpha = 1.0 / omegainvshape;
        double beta = alpha / omegamean;
        omegaarray->SetShape(alpha);
        omegaarray->SetScale(beta);
    }

    //-------------------
    // Traces and Monitors
    // ------------------

    double GetEmpiricalPosFrac() const {
        double tot = 0;
        for (int i = 0; i < Nsite; i++) {
            if ((*omegaarray)[i] > 1.0) {
                tot++;
            }
        }
        return tot / Nsite;
    }

    double GetMeanOmega() const {
        return omegaarray->GetMean();
    }

    double GetOmegaMean() const {
        return omegamean;
    }

    double GetOmegaInvShape() const {
        return omegainvshape;
    }

    void TraceOmega(ostream &os) const {
        for (int i = 0; i < GetNsite(); i++) {
            os << omegaarray->GetVal(i) << '\t';
        }
        os << '\n';
    }

    void GetSiteOmega(double *array) const {
        for (int i = 0; i < GetNsite(); i++) {
            array[i] = omegaarray->GetVal(i);
        }
    }
    void TraceHeader(ostream &os) const override {
        os << "#logprior\tlnL\tlength\t";
        os << "omegamean\tinvshape\t";
        os << "posfrac\t";
        os << "statent\t";
        os << "statent\t";
        os << "rrent\n";
    }

    void Trace(ostream &os) const override {
        os << GetLogPrior() << '\t';
        os << GetLogLikelihood() << '\t';
        os << branchlength->GetTotalLength() << '\t';
        os << omegamean << '\t';
        os << omegainvshape << '\t';
        os << GetEmpiricalPosFrac() << '\t';
        os << Random::GetEntropy(nucstat) << '\t';
        os << Random::GetEntropy(nucrelrate) << '\n';
    }

    void Monitor(ostream &os) const override {}

    void ToStream(ostream &os) const override {
        os << omegamean << '\t' << omegainvshape << '\n';
        os << *omegaarray << '\n';
        os << nucstat << '\t';
        os << nucrelrate << '\t';
        os << lambda << '\t';
        os << *branchlength << '\n';
    }

    void FromStream(istream &is) override {
        is >> omegamean >> omegainvshape;
        is >> *omegaarray;
        is >> nucstat;
        is >> nucrelrate;
        is >> lambda;
        is >> *branchlength;
    }
};
