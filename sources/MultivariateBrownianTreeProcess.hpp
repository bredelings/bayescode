#pragma once

#include "NodeArray.hpp"
#include "BranchArray.hpp"
#include "Random.hpp"
#include "CovMatrix.hpp"
#include "ContinuousData.hpp"
#include "MultivariateNormalSuffStat.hpp"

class MultivariateBrownianTreeProcess : public SimpleNodeArray<vector<double> >  {

    public:

    MultivariateBrownianTreeProcess(const NodeSelector<double>& intimetree, const CovMatrix& insigma) :
        SimpleNodeArray<vector<double>>(intimetree.GetTree()),
        timetree(intimetree),
        sigma(insigma),
        clamp(intimetree.GetNnode(), vector<bool>(insigma.GetDim(),false))  {
            Assign(GetRoot());
            Sample();
    }

    void Assign(const Link* from)   {
        (*this)[from->GetNode()->GetIndex()].assign(GetDim(), 0);
		for (const Link* link=from->Next(); link!=from; link=link->Next())	{
            Assign(link->Out());
        }
    }

    const Link *GetRoot() const { return GetTree().GetRoot(); }

    int GetDim() const {
        return sigma.GetDim();
    }

    void SetAndClamp(const ContinuousData& data, const vector<double>& rootval, int index, int fromindex)  {
        int k = 0;
        int n = 0;
        RecursiveSetAndClamp(GetRoot(), data, rootval, index, fromindex, k, n);
		cerr << data.GetCharacterName(fromindex) << " : " << n-k << " out of " << n << " missing\n";
    }

    void RecursiveSetAndClamp(const Link* from, const ContinuousData& data, const vector<double>& rootval, int index, int fromindex, int& k, int& n)   {

		if(from->isLeaf()){
			n++;
			int tax = data.GetTaxonSet()->GetTaxonIndex(from->GetNode()->GetName());
			if (tax != -1)	{
				double tmp = data.GetState(tax, fromindex);
				if (tmp != -1)	{
					k++;
                    (*this)[from->GetNode()->GetIndex()][index] = log(tmp) - rootval[index];
                    clamp[from->GetNode()->GetIndex()][index] = true;
				}
			}
			else	{
				cerr << "set and clamp : " << from->GetNode()->GetName() << " not found\n";
			}
		}
		for (const Link* link=from->Next(); link!=from; link=link->Next())	{
			RecursiveSetAndClamp(link->Out(), data, rootval, index, fromindex, k, n);
		}
	}

    void Shift(int index, double delta) {
        for (int i=0; i<GetNnode(); i++)   {
            if (i != GetRoot()->GetNode()->GetIndex())  {
                if (! clamp[i][index])  {
                    (*this)[i][index] += delta;
                }
            }
        }
    }

    /*
    void Shift(const vector<double>& d)    {
        for (int i=0; i<GetNnode(); i++)   {
            for (int j=0; j<GetDim(); j++)  {
                if (! clamp[i][j])    {
                    (*this)[i][j] += d[j];
                }
            }
        }
    }
    */

    void GetContrast(const Link* from, vector<double>& contrast) const {
            double dt = timetree.GetVal(from->Out()->GetNode()->GetIndex()) - timetree.GetVal(from->GetNode()->GetIndex());
            double scaling = sqrt(dt);
            const vector<double>& up = GetVal(from->GetNode()->GetIndex());
            const vector<double>& down = GetVal(from->Out()->GetNode()->GetIndex());
            for (int i=0; i<GetDim(); i++)  {
                contrast[i] += (up[i] - down[i]) / scaling;
            }
    }

    void Sample()   {
        RecursiveSample(GetRoot());
    }

    void RecursiveSample(const Link* from)  {
        LocalSample(from);
        for (const Link *link = from->Next(); link != from; link = link->Next()) {
            RecursiveSample(link->Out());
        }
    }

    void LocalSample(const Link* from) {
        if (from->isRoot()) {
            const vector<bool>& cl = clamp[from->GetNode()->GetIndex()];
            vector<double>& val = (*this)[from->GetNode()->GetIndex()];
            for (int i=0; i<GetDim(); i++)  {
                if (! cl[i])    {
                    val[i] = 0;
                }
            }
        }
        else    {
            double dt = timetree.GetVal(from->Out()->GetNode()->GetIndex()) - timetree.GetVal(from->GetNode()->GetIndex());
            if (dt <= 0)    {
                cerr << "error: negative time interval\n";
                exit(1);
            }
            double scaling = sqrt(dt);

            const vector<double>& initval = (*this)[from->Out()->GetNode()->GetIndex()];
            vector<double>& finalval = (*this)[from->GetNode()->GetIndex()];
            const vector<bool>& cl = clamp[from->GetNode()->GetIndex()];

            // draw multivariate normal from sigma
            vector<double> contrast(GetDim(), 0);
            sigma.MultivariateNormalSample(contrast);

            // not conditional on clamped entries
            for (int i=0; i<GetDim(); i++)  {
                if (! cl[i])    {
                    finalval[i] = initval[i] + scaling*contrast[i];
                }
            }
        }
    }

    double GetLogProb() const {
        return RecursiveGetLogProb(GetRoot());
    }

    double RecursiveGetLogProb(const Link* from) const  {
        double total = GetLocalLogProb(from);
        for (const Link *link = from->Next(); link != from; link = link->Next()) {
            total += RecursiveGetLogProb(link->Out());
        }
        return total;
    }

    double GetLocalLogProb(const Link* from) const  {

        // X_down ~ Normal(X_up, sigma*dt)
        // X = (X_down - X_up)
        // Y = (X_down - X_up)/sqrt(dt)
        // P(Y)dY = p(X)dX
        // p(X) = p(Y) dY/dX = p(Y) / sqrt(dt)^GetDim()
        // log P(X) = log P(Y) - 0.5 * GetDim() * log(dt)

        if (from->isRoot()) {
            return 0;
        }

        double dt = timetree.GetVal(from->Out()->GetNode()->GetIndex()) - timetree.GetVal(from->GetNode()->GetIndex());
        double scaling = sqrt(dt);
        vector<double> contrast(GetDim(), 0);

        const vector<double>& up = GetVal(from->GetNode()->GetIndex());
        const vector<double>& down = GetVal(from->Out()->GetNode()->GetIndex());
        for (int i=0; i<GetDim(); i++)  {
            contrast[i] = (up[i] - down[i])/scaling;
        }
        return sigma.logMultivariateNormalDensity(contrast) + 0.5*GetDim()*log(dt);
    }

    double GetNodeLogProb(const Link* from) const   {
        double total = GetLocalLogProb(from);
        for (const Link *link = from->Next(); link != from; link = link->Next()) {
            total += GetLocalLogProb(link->Out());
        }
        return total;
    }

    void GetSampleCovarianceMatrix(CovMatrix& covmat, int& n) const    {
        RecursiveGetSampleCovarianceMatrix(GetRoot(), covmat, n);
    }

    void RecursiveGetSampleCovarianceMatrix(const Link* from, CovMatrix& covmat, int& n) const  {

        if (! from->isRoot())   {
            vector<double> contrast(GetDim(), 0);
            for (int i=0; i<GetDim(); i++)  {
                for (int j=0; j<GetDim(); j++)  {
                    covmat.add(i, j, contrast[i]*contrast[j]);
                }
            }
            n++;
        }
        for (const Link *link = from->Next(); link != from; link = link->Next()) {
            RecursiveGetSampleCovarianceMatrix(link->Out(), covmat, n);
        }
    }

    void AddSuffStat(MultivariateNormalSuffStat& to) const {
        GetSampleCovarianceMatrix(to.covmat, to.n);
    }

    void GetSumOfContrasts(vector<double>& sum) const    {
        return RecursiveSumOfContrasts(GetRoot(), sum);
    }

    void RecursiveSumOfContrasts(const Link* from, vector<double>& sum) const {
        if (!from->isRoot())    {
            vector<double> contrast(GetDim(), 0);
            GetContrast(from, contrast);
            for (int i=0; i<GetDim(); i++)  {
                sum[i] += contrast[i];
            }
        }
        for (const Link *link = from->Next(); link != from; link = link->Next()) {
            RecursiveSumOfContrasts(link->Out(), sum);
        }
    }

    double LocalProposeMove(int i, int j, double tuning)  {
        if (! clamp[i][j])  {
            (*this)[i][j] += tuning * (Random::Uniform() - 0.5);
        }
        return 0;
    }

    private:

    const NodeSelector<double>& timetree;
    const CovMatrix& sigma;
    vector<vector<bool>> clamp;
};

class MVBranchExpoLengthArray : public SimpleBranchArray<double>    {

    public:

    MVBranchExpoLengthArray(const NodeSelector<vector<double>>& innodetree, const vector<double>& inrootval, const NodeSelector<double>& inchrono, int inidx) :
        SimpleBranchArray<double>(innodetree.GetTree()),
        nodetree(innodetree),
        rootval(inrootval),
        chrono(inchrono),
        idx(inidx)  {
            Update();
    }

    const Link *GetRoot() const { return GetTree().GetRoot(); }

    double GetTotalLength() const {
        return RecursiveGetTotalLength(GetRoot());
    }

    double RecursiveGetTotalLength(const Link* from) const {
        double tot = 0;
        if (! from->isRoot())   {
            tot += GetVal(from->GetBranch()->GetIndex());
        }
        for (const Link *link = from->Next(); link != from; link = link->Next()) {
            tot += RecursiveGetTotalLength(link->Out());
        }
        return tot;
    }

    void Update()   {
        RecursiveUpdate(GetRoot());
    }

    void RecursiveUpdate(const Link* from)  {
        LocalUpdate(from);
        for (const Link *link = from->Next(); link != from; link = link->Next()) {
            RecursiveUpdate(link->Out());
        }
    }

    void LocalUpdate(const Link* from)  {
        if (!from->isRoot()) {
            double up = nodetree.GetVal(from->GetNode()->GetIndex())[idx] + rootval[idx];
            double down = nodetree.GetVal(from->Out()->GetNode()->GetIndex())[idx] + rootval[idx];
            double mean = (exp(up) - exp(down)) / (up - down);
            double dt = chrono.GetVal(from->Out()->GetNode()->GetIndex()) - chrono.GetVal(from->GetNode()->GetIndex());
            if (dt <= 0)    {
                cerr << "error: negative time on chronogram\n";
                exit(1);
            }
            (*this)[from->GetBranch()->GetIndex()] = mean * dt;
        }
    }

    void LocalNodeUpdate(const Link* from)  {
        LocalUpdate(from);
        for (const Link *link = from->Next(); link != from; link = link->Next()) {
            LocalUpdate(link->Out());
        }
    }

    private:
    const NodeSelector<vector<double>>& nodetree;
    const vector<double>& rootval;
    const NodeSelector<double>& chrono;
    int idx;
};

class MVBranchExpoMeanArray : public SimpleBranchArray<double>    {

    public:

    MVBranchExpoMeanArray(const NodeSelector<vector<double>>& innodetree, const vector<double>& inrootval, int inidx) :
        SimpleBranchArray<double>(innodetree.GetTree()),
        nodetree(innodetree),
        rootval(inrootval),
        idx(inidx)  {
            Update();
    }

    const Link *GetRoot() const { return GetTree().GetRoot(); }

    double GetMean() const  {
        return GetTotal() / GetTree().GetNbranch();
    }

    double GetTotal() const {
        return RecursiveGetTotal(GetRoot());
    }

    double RecursiveGetTotal(const Link* from) const {
        double tot = 0;
        if (! from->isRoot())   {
            tot += GetVal(from->GetBranch()->GetIndex());
        }
        for (const Link *link = from->Next(); link != from; link = link->Next()) {
            tot += RecursiveGetTotal(link->Out());
        }
        return tot;
    }

    void Update()   {
        RecursiveUpdate(GetRoot());
    }

    void RecursiveUpdate(const Link* from)  {
        LocalUpdate(from);
        for (const Link *link = from->Next(); link != from; link = link->Next()) {
            RecursiveUpdate(link->Out());
        }
    }

    void LocalUpdate(const Link* from)  {
        if (!from->isRoot()) {
            double up = nodetree.GetVal(from->GetNode()->GetIndex())[idx] + rootval[idx];
            double down = nodetree.GetVal(from->Out()->GetNode()->GetIndex())[idx] + rootval[idx];
            double mean = (exp(up) - exp(down)) / (up - down);
            (*this)[from->GetBranch()->GetIndex()] = mean;
        }
    }

    void LocalNodeUpdate(const Link* from)  {
        LocalUpdate(from);
        for (const Link *link = from->Next(); link != from; link = link->Next()) {
            LocalUpdate(link->Out());
        }
    }

    private:
    const NodeSelector<vector<double>>& nodetree;
    const vector<double>& rootval;
    int idx;
};


