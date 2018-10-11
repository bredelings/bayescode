
#ifndef MULTIPROBMODEL_H
#define MULTIPROBMODEL_H

#include "MultiGeneMPIModule.hpp"
#include "ProbModel.hpp"

class MultiGeneProbModel : public ProbModel, public MultiGeneMPIModule {
  public:
    MultiGeneProbModel(int inmyid, int innprocs)
        : ProbModel(), MultiGeneMPIModule(inmyid, innprocs) {}

    virtual void Update() override {
        if (!myid) {
            MasterUpdate();
        } else {
            SlaveUpdate();
        }
    }

    virtual void PostPred(std::string name) override {
        if (!myid) {
            MasterPostPred(name);
        } else {
            SlavePostPred(name);
        }
    }

    virtual double Move() override {
        if (!myid) {
            MasterMove();
        } else {
            SlaveMove();
        }
        return 1;
    }

    virtual void FromStream(std::istream &is) override {
        if (!myid) {
            MasterFromStream(is);
        } else {
            SlaveFromStream();
        }
    }

    virtual void ToStream(std::ostream &os) const override {
        if (!myid) {
            MasterToStream(os);
        } else {
            SlaveToStream();
        }
    }

    virtual void MasterToStream(std::ostream &os) const {}
    virtual void SlaveToStream() const {}
    virtual void MasterFromStream(std::istream &is) {}
    virtual void SlaveFromStream() {}

    virtual void MasterMove() {}
    virtual void SlaveMove() {}

    virtual void MasterUpdate() {}
    virtual void SlaveUpdate() {}

    virtual void MasterPostPred(std::string name) {}
    virtual void SlavePostPred(std::string name) {}
};

#endif
