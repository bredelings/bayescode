#ifndef IIDMULTIBB_H
#define IIDMULTIBB_H

#include "Array.hpp"
#include "Random.hpp"
#include "MPIBuffer.hpp"

class IIDMultiBernBeta : public SimpleArray<vector<double > {

    IIDMultiBernBeta(int insize, const vector<double>& inpi, const vector<double>& inmean, const vector<double>& ininvconc) :  SimpleArray<vector<double> >(insize, vector<double>(inpi.size(),0)), pi(inpi), mean(inmean), invconc(ininvconc)  {
        Sample();
    }

    ~IIDMultiBernBeta() {}

    int GetDim()    {
        return pi.size();
    }

    void Sample()   {
        for (int i=0; i<GetSize(); i++) {
            for (int k=0; k<GetDim(); k++)  {
                if (Random::Uniform() < pi[k])  {
                    (*this)[i][k] = Random::BetaSample(mean[k]/invconc[k],(1-mean[k])/invconc[k]);
                }
                else    {
                    (*this)[i][k] = 0;
                }
            }
        }
    }

    double GetLogProb() {
        double total = 0;
        for (int i=0; i<GetSize(); i++) {
            total += GetLogProb(int i);
        }
        return total;
    }

    double GetLogProb(int i)    {
        double total = 0;
        for (int k=0; k<GetDim(); k++)  {
            total += GetLogProb(i,k);
        }
        return total;
    }

    double GetLogProb(int i, int k) {
        if (GetVal(i)[k])   {
            return log(pi[k]) + Random::logBetaDensity(GetVal(i)[k],mean[k]/invconc[k],(1-mean[k])/invconc[k]);
        }
        return log(1-pi[k]);
    }

    private:

    const vector<double>& pi;
    const vector<double>& mean;
    const vector<douhble>& invconc;
};


class IIDMultiCount : public SimpleArray<vector<int> >  {

    public:

    IIDMultiCount(const vector<int>& intotcount, const vector<double>& inpi, const vector<double>& inmean, const vector<double>& ininvconc) :  SimpleArray<vector<int> >(intotcount.size(), vector<int>(inpi.size(),0)), totcount(intotcount), pi(inpi), mean(inmean), invconc(ininvconc) {
        Clear();
    }

    void Clear()    {
       for (int i=0; i<GetSize(); i++)  {
          for (int k=0; k<GetDim(); k++)    {
             (*this)[i][k] = 0;
          }
       }
    }

    ~IIDMultiCount() {}

    int GetDim() const  {
        return pi.size();
    }

    douhble GetMarginalLogProb() const  {
        double total = 0;
        for (int k=0; k<GetDim(); k++)  {
            total += GetMarginalLogProb(k);
        }
        return total;
    }

    double GetMarginalLogProb(int k) const  {
        double total = 0;
        for (int i=0; i<GetSize(); i++) {
            total += GetMarginalLogProb(i,k);
        }
        return total;
    }

    double GetMarginalLogProb(int i, int k) {
        double ret = 0;
        double alpha = mean[k]/invconc[k];
        double beta = (1-mean[k])/invconc[k];
        if (GetVal(i)[k])   {
            ret = log(pi[k]) + Random::logGamma(alpha+beta) - Random::logGamma(alpha) - Random::logGamma(beta) + Random::logGamma(alpha + GetVal(i)[k]) + Random::logGamma(beta + totcount[i] - GetVal(i)[k]) - Random::logGamma(alpha + beta + totcount[i]);
        }
        else    {
            double p0 = log(1 - pi[k]);
            double p1 = log(pi[k]) + Random::logGamma(alpha+beta) - Random::logGamma(beta) + Random::logGamma(beta + totcount[i]) - Random::logGamma(alpha + beta + totcount[i]);
            double max = (p0 > p1) ? p0 : p1;
            double tot = exp(p0-max) + exp(p1-max);
            ret = log(tot) + max;
        }
        return ret;
    }

    private:

    const vector<int>& totcount;
    const vector<double>& pi;
    const vector<double>& mean;
    const vector<double>& invconc;
};

/*
class IIDMultiCount : public SimpleArray<vector<int> >  {

    public:

    IIDMultiCount(int insize, int indim) : SimpleArray<vector<int> >(insize, vector<int>(indim,0)) {}
    ~IIDMultiCount() {}

    int GetDim() const  {
        return GetVal(0).size();
    }

    double GetMarginalLogProb(int k, const vector<double>& pi, const vector<double>& mean, const vector<double>& invconc) const {
        double total = 0;
        double alpha = mean[k]/invconc[k];
        double beta = (1-mean[k])/invconc[k];
        for (int i=0; i<GetSize(); i++) {
            total += GetMarginalLogProb(i,k,pi[k],alpha,beta);
        }
        return total;
    }

    double GetMarginalLogProb(int i, int k, double pi, double alpha, double beta)    {
        double ret = 0;
        if (GetVal(i)[k])   {
            double alpha
            ret = log(pi) + Random::logGamma(alpha+beta) - Random::logGamma(alpha) - Random::logGamma(beta) + Random::logGamma(alpha + 
        }
        else    {
        }
    }

    private:

};
*/


#endif

