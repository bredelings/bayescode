
#include "AAMutSelDSBDPOmegaModel.hpp"
#include "Parallel.hpp"
#include "MultiGeneProbModel.hpp"

class Permutation : public SimpleArray<int> {

    public:

    Permutation(int size) : SimpleArray<int>(size) {}
    ~Permutation() {}

    void Reset()    {
        for (int i=0; i<GetSize(); i++) {
            (*this)[i] = i;
        }
    }

    unsigned int GetMPISize() const {return GetSize();}

    void MPIPut(MPIBuffer& buffer) const    {
        for (int i=0; i<GetSize(); i++) {
            buffer << GetVal(i);
        }
    }

    void MPIGet(const MPIBuffer& buffer)    {
        for (int i=0; i<GetSize(); i++) {
            buffer >> (*this)[i];
        }
    }
};

class MultiGeneAAMutSelDSBDPOmegaModel : public MultiGeneProbModel {

    private:

	Tree* tree;
	CodonSequenceAlignment* refcodondata;
	const TaxonSet* taxonset;

    string treefile;

	int Ntaxa;
	int Nbranch;

    int baseNcat;
    int Ncat;

	double lambda;
	BranchIIDGamma* branchlength;
	
	double omegahypermean;
	double omegahyperinvshape;
	IIDGamma* omegaarray;
	GammaSuffStat omegahypersuffstat;

	PoissonSuffStatBranchArray* lengthsuffstatarray;
	GammaSuffStat lambdasuffstat;

    // mixture components
    // set of baseNcat Dirichlet densities
    // centers

    vector<double> basecenterhypercenter;
    double basecenterhyperinvconc;
    IIDDirichlet* basecenterarray;

    double baseconchypermean;
    double baseconchyperinvshape;
    IIDGamma* baseconcentrationarray;

    DirichletSuffStatArray* basesuffstatarray;

    // now, a mixture model drawing from this set of Ncat components
    // weights:
    double basekappa;
    StickBreakingProcess* baseweight;
    OccupancySuffStat* baseoccupancy;
    Permutation* permutocc;

    std::vector<AAMutSelDSBDPOmegaModel*> geneprocess;

    double lnL;
    double GeneLogPrior;
    double MeanNcluster;
    double MeanStatEnt;
    double MeanAAConc;
    double MeanAACenterEnt;

    int fixomega;

    public:

    //-------------------
    // Construction and allocation
    //-------------------

    MultiGeneAAMutSelDSBDPOmegaModel(string datafile, string intreefile, int inNcat, int inbaseNcat, int infixomega, int inmyid, int innprocs) : MultiGeneProbModel(inmyid,innprocs) {

        AllocateAlignments(datafile);
        treefile = intreefile;
        Ncat = inNcat;
        baseNcat = inbaseNcat;
        fixomega = infixomega;

        refcodondata = new CodonSequenceAlignment(refdata, true);
        taxonset = refdata->GetTaxonSet();
        Ntaxa = refdata->GetNtaxa();

        // get tree from file (newick format)
        tree = new Tree(treefile);

        // check whether tree and data fits together
        tree->RegisterWith(taxonset);

        tree->SetIndices();
        Nbranch = tree->GetNbranch();

        if (! myid) {
            std::cerr << "number of taxa : " << Ntaxa << '\n';
            std::cerr << "number of branches : " << Nbranch << '\n';
            std::cerr << "-- Tree and data fit together\n";
        }
    }

    void Allocate() {

        lambda = 10;
        branchlength = new BranchIIDGamma(*tree,1.0,lambda);
        lengthsuffstatarray = new PoissonSuffStatBranchArray(*tree);

        omegahypermean = 1.0;
        omegahyperinvshape = 1.0;
		omegaarray = new IIDGamma(GetLocalNgene(),1.0,1.0);
        if (fixomega) {
            for (int i=0; i<GetLocalNgene(); i++)   {
                (*omegaarray)[i] = 1.0;
            }
        }

        lnL = 0;
        GeneLogPrior = 0;
        MeanStatEnt = 0;
        MeanAAConc = 0;
        MeanAACenterEnt = 0;

        basekappa = 1.0;
        baseweight = new StickBreakingProcess(baseNcat,basekappa);
        baseoccupancy = new OccupancySuffStat(baseNcat);
        permutocc = new Permutation(baseNcat);

        basecenterhypercenter.assign(Naa,1.0/Naa);
        basecenterhyperinvconc = 1.0/Naa;

        basecenterarray = new IIDDirichlet(baseNcat,basecenterhypercenter,1.0/basecenterhyperinvconc);
        basecenterarray->SetUniform();

        baseconchypermean = Naa;
        baseconchyperinvshape = 1.0;
        double alpha = 1.0 / baseconchyperinvshape;
        double beta = alpha / baseconchypermean;

        baseconcentrationarray = new IIDGamma(baseNcat,alpha,beta);
        for (int k=0; k<baseNcat; k++)  {
            (*baseconcentrationarray)[k] = 20.0;
        }

        // suff stats for component aa fitness arrays
        basesuffstatarray = new DirichletSuffStatArray(baseNcat,Naa);

        if (! GetMyid())    {
            geneprocess.assign(0,(AAMutSelDSBDPOmegaModel*) 0);
        }
        else    {
            geneprocess.assign(GetLocalNgene(),(AAMutSelDSBDPOmegaModel*) 0);

            for (int gene=0; gene<GetLocalNgene(); gene++)   {
                geneprocess[gene] = new AAMutSelDSBDPOmegaModel(GetLocalGeneName(gene),treefile,Ncat,baseNcat);
                geneprocess[gene]->SetFixBaseMix(1);
                geneprocess[gene]->SetFixOmega(fixomega);
            }
        }
    }

    void Unfold()   {

        if (! GetMyid())    {
            MasterSendGlobalBranchLengths();
            if (! fixomega) {
                MasterSendOmegaHyperParameters();
                MasterSendOmega();
            }
            MasterSendBaseMixture();
            MasterReceiveLogProbs();
        }
        else    {

            for (int gene=0; gene<GetLocalNgene(); gene++)   {
                geneprocess[gene]->Allocate();
            }

            SlaveReceiveGlobalBranchLengths();
            if (! fixomega) {
                SlaveReceiveOmegaHyperParameters();
                SlaveReceiveOmega();
            }
            SlaveReceiveBaseMixture();

            for (int gene=0; gene<GetLocalNgene(); gene++)   {
                geneprocess[gene]->UpdateMatrices();
                geneprocess[gene]->Unfold();
            }

            SlaveSendLogProbs();
        }
    }

	CodonStateSpace* GetCodonStateSpace() const {
		return (CodonStateSpace*) refcodondata->GetStateSpace();
	}

    //-------------------
    // Traces and Monitors
    //-------------------

    int GetBaseNcluster() const {
        int n = 0;
        for (int i=0; i<baseNcat; i++)  {
            if (baseoccupancy->GetVal(i))    {
                n++;
            }
        }
        return n;
    }

    void TraceHeader(ostream& os) const {

        os << "#logprior\tlnL\tlength\t";
        os << "meanomega\t";
        os << "varomega\t";
        os << "ncluster\t";
        os << "nbasecluster\t";
        os << "aastatent\t";
        os << "baseconc\t";
        os << "baseent\n";
    }

    void Trace(ostream& os) const {
		os << GetLogPrior() << '\t';
		os << GetLogLikelihood() << '\t';
        os << 3*branchlength->GetTotalLength() << '\t';
        os << omegaarray->GetMean() << '\t';
        os << omegaarray->GetVar() << '\t';
        os << MeanNcluster << '\t';
        os << GetBaseNcluster() << '\t';
        os << MeanStatEnt << '\t';
        os << MeanAAConc << '\t';
		os << MeanAACenterEnt << '\n';
		os.flush();
    }

	void Monitor(ostream& os) const {}
	void FromStream(istream& is) {}
	void ToStream(ostream& os) const {}

    //-------------------
    // Updates
    //-------------------

    void NoUpdate() {}

    //-------------------
    // Log Prior and Likelihood
    //-------------------

    double GetLogPrior() const {
		double total = GeneLogPrior;
		total += BranchLengthsHyperLogPrior();
		total += BranchLengthsLogPrior();
        if (! fixomega) {
            total += OmegaHyperLogPrior();
            total += OmegaLogPrior();
        }
        total += BaseStickBreakingHyperLogPrior();
        total += BaseStickBreakingLogPrior();
        total += BaseLogPrior();
		return total;
    }

	double BranchLengthsHyperLogPrior() const {
		return -lambda / 10;
	}

	double BranchLengthsLogPrior() const {
		return branchlength->GetLogProb();
	}

    double OmegaHyperLogPrior() const {
        double total = 0;
        total -= omegahypermean;
        total -= omegahyperinvshape;
        return total;
    }

    double OmegaLogPrior()   const {
        return omegaarray->GetLogProb();
    }

    double BaseStickBreakingHyperLogPrior() const   {
        return -basekappa/10;
    }

    double BaseStickBreakingLogPrior() const    {
        return baseweight->GetLogProb(basekappa);
    }

    double BaseLogPrior() const {
        double total = 0;
        total += basecenterarray->GetLogProb();
        total += baseconcentrationarray->GetLogProb();
        return total;
    }

    double BaseLogPrior(int k) const {
        double total = 0;
        total += basecenterarray->GetLogProb(k);
        total += baseconcentrationarray->GetLogProb(k);
        return total;
    }

    double GetLogLikelihood() const {
        return lnL;
    }

    //-------------------
    // Suff Stat Log Probs
    //-------------------

    // suff stat for moving branch lengths hyperparameter (lambda)
	double BranchLengthsHyperSuffStatLogProb() const {
		return lambdasuffstat.GetLogProb(1.0,lambda);
	}

    // suff stats for moving omega hyper parameters
    double OmegaHyperSuffStatLogProb() const {
        double alpha = 1.0 / omegahyperinvshape;
        double beta = alpha / omegahypermean;
        return omegahypersuffstat.GetLogProb(alpha,beta);
    }

    double BaseSuffStatLogProb(int k) const   {
        return basesuffstatarray->GetVal(k).GetLogProb(basecenterarray->GetVal(k),baseconcentrationarray->GetVal(k));
    }

    //-------------------
    // Log Probs for MH moves
    //-------------------

    // log prob for moving branch lengths hyperparameter (lambda)
    double BranchLengthsHyperLogProb() const {
        return BranchLengthsHyperLogPrior() + BranchLengthsHyperSuffStatLogProb();
    }

    // log prob for moving omega hyperparameters
    double OmegaHyperLogProb() const {
        return OmegaHyperLogPrior() + OmegaHyperSuffStatLogProb();
    }

    // for moving aa hyper params (aacenter and aainvconc)
    // for component k of the mixture
    double BaseLogProb(int k) const   {
        return BaseLogPrior(k) + BaseSuffStatLogProb(k);
    }

    // for moving kappa
    double BaseStickBreakingHyperLogProb() const {
        return BaseStickBreakingHyperLogPrior() + BaseStickBreakingLogPrior();
    }

    //-------------------
    // Moves
    //-------------------

    void MasterMove() override {

		int nrep = 30;

		for (int rep=0; rep<nrep; rep++)	{

            MasterReceiveBaseSuffStat();
            MoveBaseMixture(3);
            MasterSendBaseMixture();

            if (! fixomega) {
                MasterReceiveOmega();
                MoveOmegaHyperParameters();
                MasterSendOmegaHyperParameters();
            }

            MasterReceiveLengthSuffStat();
            ResampleBranchLengths();
            MoveBranchLengthsHyperParameter();
            MasterSendGlobalBranchLengths();
        }

        MasterReceiveOmega();
        MasterReceiveLogProbs();
    }

    // slave move
    void SlaveMove() override {

        GeneResampleSub(1.0);

		int nrep = 30;

		for (int rep=0; rep<nrep; rep++)	{

            GeneCollectPathSuffStat();

            MoveGeneAA();
            SlaveSendBaseSuffStat();
            SlaveReceiveBaseMixture();

            if (! fixomega) {
                MoveGeneOmegas();
                SlaveSendOmega();
                SlaveReceiveOmegaHyperParameters();
            }

            MoveGeneNucRates();

            SlaveSendLengthSuffStat();
            SlaveReceiveGlobalBranchLengths();
        }

        SlaveSendOmega();
        SlaveSendLogProbs();
    }

    void GeneResampleSub(double frac)  {

        for (int gene=0; gene<GetLocalNgene(); gene++)   {
            geneprocess[gene]->ResampleSub(frac);
        }
    }

    void GeneCollectPathSuffStat()  {
        for (int gene=0; gene<GetLocalNgene(); gene++)   {
            geneprocess[gene]->CollectSitePathSuffStat();
            geneprocess[gene]->CollectComponentPathSuffStat();
        }
    }

    void MoveGeneOmegas()  {
        for (int gene=0; gene<GetLocalNgene(); gene++)   {
            geneprocess[gene]->MoveOmega();
            (*omegaarray)[gene] = geneprocess[gene]->GetOmega();
        }
    }

    void MoveGeneAA()   {
        for (int gene=0; gene<GetLocalNgene(); gene++)   {
            geneprocess[gene]->MoveAAMixture(3);
        }
    }

    void MoveGeneNucRates() {
        for (int gene=0; gene<GetLocalNgene(); gene++)   {
            geneprocess[gene]->MoveNucRates();
        }
    }

    void MoveBaseMixture(int nrep)    {
        for (int rep=0; rep<nrep; rep++)  {
            MoveBaseComponents(10);
            ResampleBaseEmptyComponents();
            if (baseNcat > 1)   {
                BaseLabelSwitchingMove();
                ResampleBaseWeights();
                MoveBaseKappa();
            }
        }
    }

    void MoveBaseComponents(int nrep) {

        for (int rep=0; rep<nrep; rep++)    {
            MoveBaseCenters(1.0,1);
            MoveBaseCenters(1.0,3);
            MoveBaseCenters(0.3,3);
            MoveBaseConcentrations(1.0);
            MoveBaseConcentrations(0.3);
        }
    }

    double MoveBaseCenters(double tuning, int n) {
		double nacc = 0;
		double ntot = 0;
        vector<double> bk(Naa,0);
        for (int k=0; k<baseNcat; k++)  {
            if (baseoccupancy->GetVal(k))  {
                vector<double>& aa = (*basecenterarray)[k];
                bk = aa;
                double deltalogprob = -BaseLogProb(k);
                double loghastings = Random::ProfileProposeMove(aa,Naa,tuning,n);
                deltalogprob += loghastings;
                deltalogprob += BaseLogProb(k);
                int accepted = (log(Random::Uniform()) < deltalogprob);
                if (accepted)	{
                    nacc ++;
                }
                else	{
                    aa = bk;
                }
                ntot++;
            }
        }
		return nacc/ntot;
	}

    double MoveBaseConcentrations(double tuning)  {
		double nacc = 0;
		double ntot = 0;
        for (int k=0; k<baseNcat; k++)  {
            if (baseoccupancy->GetVal(k))  {
                double& c = (*baseconcentrationarray)[k];
                double bk = c;
                double deltalogprob = -BaseLogProb(k);
                double m = tuning * (Random::Uniform() - 0.5);
                double e = exp(m);
                c *= e;
                deltalogprob += m;
                deltalogprob += BaseLogProb(k);
                int accepted = (log(Random::Uniform()) < deltalogprob);
                if (accepted)	{
                    nacc ++;
                }
                else	{
                    c = bk;
                }
                ntot++;
            }
        }
		return nacc/ntot;
    }

    void ResampleBaseEmptyComponents()  {
        basecenterarray->PriorResample(*baseoccupancy);
        baseconcentrationarray->PriorResample(*baseoccupancy);
    }

    void BaseLabelSwitchingMove()   {
        MoveBaseOccupiedCompAlloc(5);
        MoveBaseAdjacentCompAlloc(5);
    }

    double MoveBaseOccupiedCompAlloc(int k0)	{

        const vector<double>& w = baseweight->GetArray();

        int nrep = (int) (k0 * basekappa);
        ResampleBaseWeights();
        double total = 0.0;
        int Nocc = GetBaseNcluster();
        if (Nocc != 1)	{
            for (int i=0; i<nrep; i++)	{
                int occupiedComponentIndices[Nocc];
                int j=0;
                for (int k=0; k<baseNcat; k++)	{
                    if ((*baseoccupancy)[k] != 0)	{
                        occupiedComponentIndices[j] = k;
                        j++;
                    }
                }
                if (j != Nocc)	{
                    cerr << "error in MoveOccupiedCompAlloc.\n";
                    exit(1);
                }
                int indices[2];
                Random::DrawFromUrn(indices,2,Nocc);
                int cat1 = occupiedComponentIndices[indices[0]];
                int cat2 = occupiedComponentIndices[indices[1]];
                double logMetropolis = ((*baseoccupancy)[cat2] - (*baseoccupancy)[cat1]) * log(w[cat1] / w[cat2]);
                int accepted = (log(Random::Uniform()) < logMetropolis);
                if (accepted)	{
                    total += 1.0;
                    basecenterarray->Swap(cat1,cat2);
                    baseconcentrationarray->Swap(cat1,cat2);
                    baseoccupancy->Swap(cat1,cat2);
                    basesuffstatarray->Swap(cat1,cat2);
                    permutocc->Swap(cat1,cat2);
                }
            }
            return total /= nrep;
        }
        return 0;
    }

    double MoveBaseAdjacentCompAlloc(int k0)	{

        ResampleBaseWeights();
        int nrep = (int) (k0 * basekappa);
        
        double total = 0;

        const vector<double>& V = baseweight->GetBetaVariates();

        for (int i=0; i<nrep; i++)	{
            int cat1 = (int)(Random::Uniform() * (baseNcat-2));  
            int cat2 = cat1 + 1;
            double logMetropolis = ((*baseoccupancy)[cat1] * log(1 - V[cat2])) - ((*baseoccupancy)[cat2] * log(1-V[cat1]));
            int accepted = (log(Random::Uniform()) < logMetropolis);
            if (accepted)	{
                total += 1.0;
                basecenterarray->Swap(cat1,cat2);
                baseconcentrationarray->Swap(cat1,cat2);
                baseoccupancy->Swap(cat1,cat2);
                baseweight->SwapComponents(cat1,cat2);
                basesuffstatarray->Swap(cat1,cat2);
                permutocc->Swap(cat1,cat2);
            }
        }

        return total /= nrep;
    }

    void ResampleBaseWeights()  {
        baseweight->GibbsResample(*baseoccupancy);
    }

    void MoveBaseKappa()    {
        ScalingMove(basekappa,1.0,10,&MultiGeneAAMutSelDSBDPOmegaModel::BaseStickBreakingHyperLogProb,&MultiGeneAAMutSelDSBDPOmegaModel::NoUpdate,this);
        ScalingMove(basekappa,0.3,10,&MultiGeneAAMutSelDSBDPOmegaModel::BaseStickBreakingHyperLogProb,&MultiGeneAAMutSelDSBDPOmegaModel::NoUpdate,this);
        baseweight->SetKappa(basekappa);
    }

    void ResampleBranchLengths()    {
		branchlength->GibbsResample(*lengthsuffstatarray);
    }

	void MoveBranchLengthsHyperParameter()	{

		lambdasuffstat.Clear();
		branchlength->AddSuffStat(lambdasuffstat);

        ScalingMove(lambda,1.0,10,&MultiGeneAAMutSelDSBDPOmegaModel::BranchLengthsHyperLogProb,&MultiGeneAAMutSelDSBDPOmegaModel::NoUpdate,this);
        ScalingMove(lambda,0.3,10,&MultiGeneAAMutSelDSBDPOmegaModel::BranchLengthsHyperLogProb,&MultiGeneAAMutSelDSBDPOmegaModel::NoUpdate,this);

		branchlength->SetScale(lambda);
    }

    void MoveOmegaHyperParameters()  {

		omegahypersuffstat.Clear();
		omegaarray->AddSuffStat(omegahypersuffstat);

        ScalingMove(omegahypermean,1.0,10,&MultiGeneAAMutSelDSBDPOmegaModel::OmegaHyperLogProb,&MultiGeneAAMutSelDSBDPOmegaModel::NoUpdate,this);
        ScalingMove(omegahypermean,0.3,10,&MultiGeneAAMutSelDSBDPOmegaModel::OmegaHyperLogProb,&MultiGeneAAMutSelDSBDPOmegaModel::NoUpdate,this);
        ScalingMove(omegahyperinvshape,1.0,10,&MultiGeneAAMutSelDSBDPOmegaModel::OmegaHyperLogProb,&MultiGeneAAMutSelDSBDPOmegaModel::NoUpdate,this);
        ScalingMove(omegahyperinvshape,0.3,10,&MultiGeneAAMutSelDSBDPOmegaModel::OmegaHyperLogProb,&MultiGeneAAMutSelDSBDPOmegaModel::NoUpdate,this);

        double alpha = 1.0 / omegahyperinvshape;
        double beta = alpha / omegahypermean;
        omegaarray->SetShape(alpha);
        omegaarray->SetScale(beta);
    }

    //-------------------
    // MPI send / receive
    //-------------------

    // global branch lengths
    
    void MasterSendGlobalBranchLengths() {
        MasterSendGlobal(*branchlength);
    }

    void SlaveReceiveGlobalBranchLengths()   {
        SlaveReceiveGlobal(*branchlength);
        for (int gene=0; gene<GetLocalNgene(); gene++)   {
            geneprocess[gene]->SetBranchLengths(*branchlength);
        }
    }

    // omega (and hyperparameters)

    void SlaveSendOmega()   {
        SlaveSendGeneArray(*omegaarray);
    }

    void MasterReceiveOmega()    {
        MasterReceiveGeneArray(*omegaarray);
    }

    void MasterSendOmega()  {
        MasterSendGeneArray(*omegaarray);
    }
    
    void SlaveReceiveOmega()    {
        SlaveReceiveGeneArray(*omegaarray);
    }

    // omega hyperparameters

    void MasterSendOmegaHyperParameters()   {
        MasterSendGlobal(omegahypermean,omegahyperinvshape);
    }

    void SlaveReceiveOmegaHyperParameters() {
        SlaveReceiveGlobal(omegahypermean,omegahyperinvshape);
        for (int gene=0; gene<GetLocalNgene(); gene++)    {
            geneprocess[gene]->SetOmegaHyperParameters(omegahypermean,omegahyperinvshape);
        }
    }

    // aa base mixture

    void MasterSendBaseMixture() {
        MasterSendGlobal(*basecenterarray,*baseconcentrationarray);
        MasterSendGlobal(*baseweight,*permutocc);
    }

    void SlaveReceiveBaseMixture()   {
        SlaveReceiveGlobal(*basecenterarray,*baseconcentrationarray);
        SlaveReceiveGlobal(*baseweight,*permutocc);
        for (int gene=0; gene<GetLocalNgene(); gene++)    {
            geneprocess[gene]->SetBaseMixture(*basecenterarray,*baseconcentrationarray,*baseweight,*permutocc);
        }
    }

    // aa hyper suff stat

    void SlaveSendBaseSuffStat() {
        basesuffstatarray->Clear();
        baseoccupancy->Clear();
        for (int gene=0; gene<GetLocalNgene(); gene++)   {
            geneprocess[gene]->CollectBaseSuffStat();
            basesuffstatarray->Add(*geneprocess[gene]->GetBaseSuffStatArray());
            geneprocess[gene]->UpdateBaseOccupancies();
            baseoccupancy->Add(*geneprocess[gene]->GetBaseOccupancies());
        }
        SlaveSendAdditive(*basesuffstatarray);
        SlaveSendAdditive(*baseoccupancy);
    }

    void MasterReceiveBaseSuffStat() {

        basesuffstatarray->Clear();
        baseoccupancy->Clear();
        MasterReceiveAdditive(*basesuffstatarray);
        MasterReceiveAdditive(*baseoccupancy);
    }

    // branch length suff stat

    void SlaveSendLengthSuffStat()  {
        lengthsuffstatarray->Clear();
        for (int gene=0; gene<GetLocalNgene(); gene++)   {
            geneprocess[gene]->CollectLengthSuffStat();
            lengthsuffstatarray->Add(*geneprocess[gene]->GetLengthSuffStatArray());
        }
        SlaveSendAdditive(*lengthsuffstatarray);
    }

    void MasterReceiveLengthSuffStat()  {
        lengthsuffstatarray->Clear();
        MasterReceiveAdditive(*lengthsuffstatarray);
    }

    // log probs

    void SlaveSendLogProbs()   {

        GeneLogPrior = 0;
        lnL = 0;
        MeanStatEnt = 0;
        MeanAAConc = 0;
        MeanAACenterEnt = 0;
        for (int gene=0; gene<GetLocalNgene(); gene++)   {
            GeneLogPrior += geneprocess[gene]->GetLogPrior();
            lnL += geneprocess[gene]->GetLogLikelihood();
            MeanNcluster += geneprocess[gene]->GetNcluster();
            MeanStatEnt += geneprocess[gene]->GetNsite() * geneprocess[gene]->GetMeanAAEntropy();
            MeanAAConc += geneprocess[gene]->GetNsite() * geneprocess[gene]->GetMeanComponentAAConcentration();
            MeanAACenterEnt += geneprocess[gene]->GetNsite() * geneprocess[gene]->GetMeanComponentAAEntropy();
        }
        SlaveSendAdditive(GeneLogPrior);
        SlaveSendAdditive(lnL);
        SlaveSendAdditive(MeanNcluster);
        SlaveSendAdditive(MeanStatEnt);
        SlaveSendAdditive(MeanAAConc);
        SlaveSendAdditive(MeanAACenterEnt);
    }

    void MasterReceiveLogProbs()    {

        GeneLogPrior = 0;
        lnL = 0;
        MasterReceiveAdditive(GeneLogPrior);
        MasterReceiveAdditive(lnL);
        MeanNcluster = 0;
        MeanStatEnt = 0;
        MeanAAConc = 0;
        MeanAACenterEnt = 0;
        MasterReceiveAdditive(MeanNcluster);
        MasterReceiveAdditive(MeanStatEnt);
        MasterReceiveAdditive(MeanAAConc);
        MasterReceiveAdditive(MeanAACenterEnt);
        MeanNcluster /= GetLocalNgene();
        MeanStatEnt /= GetTotNsite();
        MeanAAConc /= GetTotNsite();
        MeanAACenterEnt /= GetTotNsite();
    }
};

