
#include "CodonSequenceAlignment.hpp"
#include "Tree.hpp"
#include "ProbModel.hpp"
#include "GTRSubMatrix.hpp"
#include "AAMutSelOmegaCodonSubMatrix.hpp"
#include "PhyloProcess.hpp"
#include "IIDGamma.hpp"
#include "IIDDirichlet.hpp"
#include "CodonSuffStat.hpp"

const int Nrr = Nnuc * (Nnuc-1) / 2;
const int Nstate = 61;

class AAMutSelOmegaModel {

	Tree* tree;
	FileSequenceAlignment* data;
	const TaxonSet* taxonset;
	CodonSequenceAlignment* codondata;

	int Nsite;
	int Ntaxa;
	int Nbranch;

	double lambda;
	BranchIIDGamma* branchlength;
	PoissonSuffStatBranchArray* lengthsuffstatarray;
	GammaSuffStat lambdasuffstat;

	std::vector<double> nucstat;
	std::vector<double> nucrelrate;
	GTRSubMatrix* nucmatrix;

    double alpha;
    double beta;
	double omega;
	OmegaSuffStat omegasuffstat;
	
    vector<double> aacenter;
    double aainvconc;
    IIDDirichlet* aafitnessarray;
	AAMutSelOmegaCodonSubMatrixArray* codonmatrixarray;

	PhyloProcess* phyloprocess;

	PathSuffStatArray* pathsuffstatarray;
    // NucPathSuffStat nucpathsuffstat;


	public:

	AAMutSelOmegaModel(string datafile, string treefile)  {

		data = new FileSequenceAlignment(datafile);
		codondata = new CodonSequenceAlignment(data, true);

		Nsite = codondata->GetNsite();    // # columns
		Ntaxa = codondata->GetNtaxa();

		std::cerr << "-- Number of sites: " << Nsite << std::endl;

		taxonset = codondata->GetTaxonSet();

		// get tree from file (newick format)
		tree = new Tree(treefile);

		// check whether tree and data fits together
		tree->RegisterWith(taxonset);

		tree->SetIndices();
		Nbranch = tree->GetNbranch();

		// Allocate();
	}

    void Unfold()   {

		cerr << "-- unfold\n";
		phyloprocess->Unfold();
		cerr << phyloprocess->GetLogProb() << '\n';
		std::cerr << "-- mapping substitutions\n";
		phyloprocess->ResampleSub();
		// Trace(cerr);
    }

	CodonStateSpace* GetCodonStateSpace()   {
		return (CodonStateSpace*) codondata->GetStateSpace();
	}

	void Allocate()	{

		lambda = 10;
		branchlength = new BranchIIDGamma(*tree,1.0,lambda);
		lengthsuffstatarray = new PoissonSuffStatBranchArray(*tree);

		nucrelrate.assign(Nrr,0);
		double totrr = 0;
		for (int k=0; k<Nrr; k++)	{
			nucrelrate[k] = Random::sExpo();
			totrr += nucrelrate[k];
		}
		for (int k=0; k<Nrr; k++)	{
			nucrelrate[k] /= totrr;
		}

		nucstat.assign(Nnuc,0);
		double totstat = 0;
		for (int k=0; k<Nnuc; k++)	{
			nucstat[k] = Random::sGamma(1.0);
			totstat += nucstat[k];
		}
		for (int k=0; k<Nnuc; k++)	{
			nucstat[k] /= totstat;
		}
		nucmatrix = new GTRSubMatrix(Nnuc,nucrelrate,nucstat,true);

        aacenter.assign(Naa,1.0/Naa);
        aainvconc = 1.0/Naa;
        aafitnessarray = new IIDDirichlet(Nsite,aacenter,1.0/aainvconc);

        alpha = beta = 1.0;
		omega = 1.0;

        codonmatrixarray = new AAMutSelOmegaCodonSubMatrixArray(GetCodonStateSpace(), nucmatrix, aafitnessarray, omega);
		phyloprocess = new PhyloProcess(tree,codondata,branchlength,0,codonmatrixarray);
		pathsuffstatarray = new PathSuffStatArray(Nsite);
	}

    void SetBranchLengths(const ConstBranchArray<double>& inbranchlength)    {
        for (int j=0; j<Nbranch; j++)   {
            (*branchlength)[j] = inbranchlength.GetVal(j);
        }
    }

    void SetOmega(double inomega)   {
        omega = inomega;
        UpdateCodonMatrices();
    }

    void SetAlphaBeta(double inalpha, double inbeta)    {
        alpha = inalpha;
        beta = inbeta;
    }

    void SetNucRates(const std::vector<double>& innucrelrate, const std::vector<double>& innucstat) {
        for (int j=0; j<Nrr; j++)   {
            nucrelrate[j] = innucrelrate[j];
        }
        for (int j=0; j<Nnuc; j++)  {
            nucstat[j] = innucstat[j];
        }
        UpdateMatrices();
    }

    double GetOmega()   {
        return omega;
    }

    const PoissonSuffStatBranchArray* GetLengthSuffStatArray()  {
        return lengthsuffstatarray;
    }

    /*
    const NucPathSuffStat& GetNucPathSuffStat() {
        return nucpathsuffstat;
    }
    */

	void UpdateNucMatrix()	{
		nucmatrix->CopyStationary(nucstat);
		nucmatrix->CorruptMatrix();
	}

	void UpdateCodonMatrices()	{
        codonmatrixarray->SetOmega(omega);
		codonmatrixarray->UpdateCodonMatrices();
	}

    void UpdateCodonMatrix(int site)    {
        (*codonmatrixarray)[site].CorruptMatrix();
    }
		
    /*
	void UpdateCodonMatrix()	{
		codonmatrix->SetOmega(omega);
		codonmatrix->CorruptMatrix();
	}
    */
		
    void UpdateMatrices()   {
        UpdateNucMatrix();
        UpdateCodonMatrices();
    }

	double PathSuffStatLogProb()	{
		return pathsuffstatarray->GetLogProb(*codonmatrixarray);
	}

    double PathSuffStatLogProb(int site)    {
        return pathsuffstatarray->GetVal(site).GetLogProb(codonmatrixarray->GetVal(site));
    }

	// exponential of mean 1
	double OmegaLogProb()	{
		return alpha * log(beta) - Random::logGamma(alpha) + (alpha-1) * log(omega) - beta*omega;
	}

    double AALogProb()  {
        return aafitnessarray->GetLogProb();
    }

    double AALogPrior(int i)    {
        return aafitnessarray->GetLogProb(i);
    }

	double LambdaLogProb()	{
		return -lambda / 10;
	}

	double LengthSuffStatLogProb()	{
		return lambdasuffstat.GetLogProb(1.0,lambda);
	}

	double LengthLogProb()	{
		return branchlength->GetLogProb();
	}

	void Move()	{

		ResampleSub();

		int nrep = 30;

		for (int rep=0; rep<nrep; rep++)	{

			ResampleBranchLengths();
			MoveLambda();

			CollectPathSuffStat();

            MoveAA();
			// MoveOmega();
			MoveNuc();
		}
	}

    void ResampleSub()  {
        UpdateMatrices();
		phyloprocess->ResampleSub();
    }

    void CollectLengthSuffStat()    {
		lengthsuffstatarray->Clear();
		phyloprocess->AddLengthSuffStat(*lengthsuffstatarray);
    }

	void ResampleBranchLengths()	{
        CollectLengthSuffStat();
		branchlength->GibbsResample(*lengthsuffstatarray);
	}

	void MoveLambda()	{

		lambdasuffstat.Clear();
		branchlength->AddSuffStat(lambdasuffstat);
		MoveLambda(1.0,10);
		MoveLambda(0.3,10);
		branchlength->SetScale(lambda);
	}

	void CollectPathSuffStat()	{

		pathsuffstatarray->Clear();
		phyloprocess->AddPathSuffStat(*pathsuffstatarray);
	}

	void MoveOmega()	{

        /*
		omegasuffstat.Clear();
		omegasuffstat.AddSuffStat(*codonmatrix,pathsuffstat);
		omega = Random::Gamma(alpha + omegasuffstat.GetCount(), beta + omegasuffstat.GetBeta());
		UpdateCodonMatrix();
        */
	}

    double NucPathSuffStatLogProb() {
        return PathSuffStatLogProb();
        // return nucpathsuffstat.GetLogProb(*nucmatrix,*GetCodonStateSpace());
    }

    void CollectNucPathSuffStat()    {
        UpdateMatrices();
        /*
        nucpathsuffstat.Clear();
        nucpathsuffstat.AddSuffStat(*codonmatrix,pathsuffstat);
        */
    }

	void MoveNuc()	{

        CollectNucPathSuffStat();

		MoveRR(0.1,1,3);
		MoveRR(0.03,3,3);
		MoveRR(0.01,3,3);

		MoveNucStat(0.1,1,3);
		MoveNucStat(0.01,1,3);

        UpdateMatrices();
	}

	double MoveRR(double tuning, int n, int nrep)	{
		double nacc = 0;
		double ntot = 0;
		double bk[Nrr];
		for (int rep=0; rep<nrep; rep++)	{
			for (int l=0; l<Nrr; l++)	{
				bk[l] = nucrelrate[l];
			}
			double deltalogprob = -NucPathSuffStatLogProb();
			double loghastings = Random::ProfileProposeMove(nucrelrate,Nrr,tuning,n);
			deltalogprob += loghastings;
            UpdateNucMatrix();
			deltalogprob += NucPathSuffStatLogProb();
			int accepted = (log(Random::Uniform()) < deltalogprob);
			if (accepted)	{
				nacc ++;
			}
			else	{
				for (int l=0; l<Nrr; l++)	{
					nucrelrate[l] = bk[l];
				}
                UpdateNucMatrix();
			}
			ntot++;
		}
		return nacc/ntot;
	}

	double MoveNucStat(double tuning, int n, int nrep)	{
		double nacc = 0;
		double ntot = 0;
		double bk[Nnuc];
		for (int rep=0; rep<nrep; rep++)	{
			for (int l=0; l<Nnuc; l++)	{
				bk[l] = nucstat[l];
			}
			double deltalogprob = -NucPathSuffStatLogProb();
			double loghastings = Random::ProfileProposeMove(nucstat,Nnuc,tuning,n);
			deltalogprob += loghastings;
            UpdateNucMatrix();
			deltalogprob += NucPathSuffStatLogProb();
			int accepted = (log(Random::Uniform()) < deltalogprob);
			if (accepted)	{
				nacc ++;
			}
			else	{
				for (int l=0; l<Nnuc; l++)	{
					nucstat[l] = bk[l];
				}
                UpdateNucMatrix();
			}
			ntot++;
		}
		return nacc/ntot;
	}

    double MoveAA() {
        MoveAA(1.0,1,3);
        MoveAA(0.3,1,3);
        MoveAA(0.1,3,3);
        MoveAA(0.1,5,3);
        return 1.0;
    }

	double MoveAA(double tuning, int n, int nrep)	{
		double nacc = 0;
		double ntot = 0;
		double bk[Naa];
        for (int i=0; i<Nsite; i++) {
            vector<double>& aa = (*aafitnessarray)[i];
            for (int rep=0; rep<nrep; rep++)	{
                for (int l=0; l<Naa; l++)	{
                    bk[l] = aa[l];
                }
                double deltalogprob = -AALogPrior(i) - PathSuffStatLogProb(i);
                double loghastings = Random::ProfileProposeMove(aa,Naa,tuning,n);
                deltalogprob += loghastings;
                UpdateCodonMatrix(i);
                deltalogprob += PathSuffStatLogProb(i);
                int accepted = (log(Random::Uniform()) < deltalogprob);
                if (accepted)	{
                    nacc ++;
                }
                else	{
                    for (int l=0; l<Naa; l++)	{
                        aa[l] = bk[l];
                    }
                    UpdateCodonMatrix(i);
                }
                ntot++;
            }
        }
		return nacc/ntot;
	}

	double MoveLambda(double tuning, int nrep)	{

		double nacc = 0;
		double ntot = 0;
		for (int rep=0; rep<nrep; rep++)	{
			double deltalogprob = - LambdaLogProb() - LengthSuffStatLogProb();
			double m = tuning * (Random::Uniform() - 0.5);
			double e = exp(m);
			lambda *= e;
			deltalogprob += LambdaLogProb() + LengthSuffStatLogProb();
			deltalogprob += m;
			int accepted = (log(Random::Uniform()) < deltalogprob);
			if (accepted)	{
				nacc ++;
			}
			else	{
				lambda /= e;
			}
			ntot++;
		}
		return nacc/ntot;
	}

	// summary statistics

	double GetTotalLength()	{
		double tot = 0;
		for (int j=1; j<Nbranch; j++)	{
			tot += branchlength->GetVal(j);
		}
		return tot;
	}

	double GetLogPrior() {
		double total = 0;
		total += LambdaLogProb();
		total += LengthLogProb();
        total += AALogProb();
		total += OmegaLogProb();
		return total;
	}

	double GetLogLikelihood()	{
		return phyloprocess->GetLogProb();
	}

	void TraceHeader(std::ostream& os)  {
		os << "#logprior\tlnL\tlength\tlambda\t";
        os << "aaent\t";
		os << "omega\t";
		os << "statent\t";
		os << "rrent\n";
	}

	void Trace(ostream& os) {	
		os << GetLogPrior() << '\t';
		os << GetLogLikelihood() << '\t';
		os << GetTotalLength() << '\t';
		os << lambda << '\t';
        os << aafitnessarray->GetMeanEntropy() << '\t';
		os << omega << '\t';
		os << Random::GetEntropy(nucstat) << '\t';
		os << Random::GetEntropy(nucrelrate) << '\n';
	}

	void Monitor(ostream& os) {}

	void FromStream(istream& is) {}
	void ToStream(ostream& os) {}

};


