#ifndef XGBOOST_REG_H
#define XGBOOST_REG_H
/*!
* \file xgboost_reg.h
* \brief class for gradient boosted regression
*/
#include <cmath>
#include <cstdlib>
#include <cstring>
#include "dmatrix.h"
#include "evaluation.h"
#include "../utils/omp.h"
#include "../gbm/gbtree-inl.h"
#include "../utils/utils.h"
#include "../utils/io.h"

namespace xgboost {
namespace learner {
/*! \brief class for gradient boosted regression */
class BoostLearner {
 public:
  /*! \brief constructor */
  BoostLearner(void) {
    silent = 0; 
  }
  /*! 
  * \brief a regression booter associated with training and evaluating data 
  * \param train pointer to the training data
  * \param evals array of evaluating data
  * \param evname name of evaluation data, used print statistics
  */
  BoostLearner(const DMatrix *train,
               const std::vector<DMatrix *> &evals,
               const std::vector<std::string> &evname) {
    silent = 0;
    this->SetData(train, evals, evname);
  }

  /*! 
  * \brief associate regression booster with training and evaluating data 
  * \param train pointer to the training data
  * \param evals array of evaluating data
  * \param evname name of evaluation data, used print statistics
  */
  inline void SetData(const DMatrix *train,
                      const std::vector<DMatrix *> &evals,
                      const std::vector<std::string> &evname) {
    this->train_ = train;
    this->evals_ = evals;
    this->evname_ = evname; 
    // estimate feature bound
    int num_feature = (int)(train->data.NumCol());
    // assign buffer index
    unsigned buffer_size = static_cast<unsigned>(train->Size());
    for (size_t i = 0; i < evals.size(); ++i) {
      buffer_size += static_cast<unsigned>(evals[i]->Size());
      num_feature = std::max(num_feature, (int)(evals[i]->data.NumCol()));
    }
    
    char str_temp[25];
    if (num_feature > mparam.num_feature) {
      mparam.num_feature = num_feature;
      sprintf(str_temp, "%d", num_feature);
      base_gbm.SetParam("bst:num_feature", str_temp);
    }
    sprintf(str_temp, "%u", buffer_size);
    base_gbm.SetParam("num_pbuffer", str_temp);
    if (!silent) {
      printf("buffer_size=%u\n", buffer_size);
    }
    
    // set eval_preds tmp sapce
    this->eval_preds_.resize(evals.size(), std::vector<float>());
  }
  /*! 
   * \brief set parameters from outside 
   * \param name name of the parameter
   * \param val  value of the parameter
   */
  inline void SetParam(const char *name, const char *val) {
    if(!strcmp(name, "silent")) silent = atoi(val);
    if(!strcmp(name, "eval_metric")) evaluator_.AddEval(val);                
    mparam.SetParam(name, val);
    base_gbm.SetParam(name, val);
  }
  /*!
   * \brief initialize solver before training, called before training
   * this function is reserved for solver to allocate necessary space and do other preparation 
   */
  inline void InitTrainer(void) {
    base_gbm.InitTrainer();
    if (mparam.loss_type == kLogisticClassify) {
      evaluator_.AddEval("error");
    } else {
      evaluator_.AddEval("rmse");
    }
    evaluator_.Init();
  } 
  /*! 
   * \brief save model to stream
   * \param fo output stream
   */
  inline void SaveModel(utils::IStream &fo) const {
    base_gbm.SaveModel(fo);	
    fo.Write(&mparam, sizeof(ModelParam));
  } 
  /*! 
   * \brief load model from stream
   * \param fi input stream
   */          
  inline void LoadModel(utils::IStream &fi) {
    base_gbm.LoadModel(fi);
    utils::Assert(fi.Read(&mparam, sizeof(ModelParam)) != 0);
  }
  /*!
   * \brief initialize the current data storage for model, if the model is used first time, call this function
   */
  inline void InitModel(void) {
    base_gbm.InitModel();
    mparam.AdjustBase();
  } 
  /*! 
   * \brief update the model for one iteration
   * \param iteration iteration number
   */
  inline void UpdateOneIter(int iter) {
    this->PredictBuffer(preds_, *train_, 0);
    this->GetGradient(preds_, train_->labels, grad_, hess_);
    std::vector<unsigned> root_index;
    base_gbm.DoBoost(grad_, hess_, train_->data, root_index);                
  }  
  
 protected:
  /*! \brief get the transformed predictions, given data */
  inline void PredictBuffer(std::vector<float> &preds, const DMatrix &data, unsigned buffer_offset) {
    preds.resize(data.Size());

    const unsigned ndata = static_cast<unsigned>(data.Size());
    #pragma omp parallel for schedule(static)
    for (unsigned j = 0; j < ndata; ++j) {                
      preds[j] = mparam.PredTransform(mparam.base_score 
          + base_gbm.Predict(data.data, j, buffer_offset + j));
    }
  }  
  /*! \brief get the first order and second order gradient, given the transformed predictions and labels */
  inline void GetGradient(const std::vector<float> &preds, 
                          const std::vector<float> &labels, 
                          std::vector<float> &grad,
                          std::vector<float> &hess) {
    grad.resize(preds.size()); hess.resize(preds.size());

    const unsigned ndata = static_cast<unsigned>(preds.size());
    #pragma omp parallel for schedule(static)
    for (unsigned j = 0; j < ndata; ++j) {
      grad[j] = mparam.FirstOrderGradient(preds[j], labels[j]);
      hess[j] = mparam.SecondOrderGradient(preds[j], labels[j]);
    }
  }           
 protected:
  enum LossType {
    kLinearSquare = 0,
    kLogisticNeglik = 1,
    kLogisticClassify = 2
  };
  /*! \brief training parameter for regression */
  struct ModelParam {
    /* \brief global bias */
    float base_score;
    /* \brief type of loss function */
    int loss_type;
    /* \brief number of features  */
    int num_feature;
    /*! \brief reserved field */
    int reserved[16];
    /*! \brief constructor */
    ModelParam(void) {
      base_score = 0.5f;
      loss_type = 0;
      num_feature = 0;
      memset(reserved, 0, sizeof(reserved));
    }
    /*! 
     * \brief set parameters from outside 
     * \param name name of the parameter
     * \param val  value of the parameter
     */
    inline void SetParam(const char *name, const char *val) {
      if (!strcmp("base_score", name)) base_score = (float)atof(val);
      if (!strcmp("loss_type", name)) loss_type = atoi(val);
      if (!strcmp("bst:num_feature", name)) num_feature = atoi(val);
    }
    /*! 
     * \brief adjust base_score
     */                
    inline void AdjustBase(void) {
      if (loss_type == 1 || loss_type == 2) {
        utils::Assert(base_score > 0.0f && base_score < 1.0f, "sigmoid range constrain");
        base_score = - logf(1.0f/base_score - 1.0f);
      }
    }
    /*! 
     * \brief transform the linear sum to prediction 
     * \param x linear sum of boosting ensemble
     * \return transformed prediction
     */
    inline float PredTransform(float x) {
      switch (loss_type) {                        
        case kLinearSquare: return x;
        case kLogisticClassify:
        case kLogisticNeglik: return 1.0f/(1.0f + expf(-x));
        default: utils::Error("unknown loss_type"); return 0.0f;
      }
    }
    /*! 
     * \brief calculate first order gradient of loss, given transformed prediction
     * \param predt transformed prediction
     * \param label true label
     * \return first order gradient
     */
    inline float FirstOrderGradient(float predt, float label) const {
      switch (loss_type) {                        
      case kLinearSquare: return predt - label;
      case kLogisticClassify:
      case kLogisticNeglik: return predt - label;
      default: utils::Error("unknown loss_type"); return 0.0f;
      }
    }
    /*! 
    * \brief calculate second order gradient of loss, given transformed prediction
    * \param predt transformed prediction
    * \param label true label
    * \return second order gradient
    */
    inline float SecondOrderGradient(float predt, float label) const {
      switch (loss_type) {                        
      case kLinearSquare: return 1.0f;
      case kLogisticClassify:
      case kLogisticNeglik: return predt * ( 1 - predt );
      default: utils::Error("unknown loss_type"); return 0.0f;
      }
    }
  };
                
  // silent during training
  int silent;
  gbm::GBTree base_gbm;
  const DMatrix *train_;
  std::vector<DMatrix *> evals_;
  std::vector<std::string> evname_;
  // model parameter
  ModelParam mparam;

 private:
  EvalSet evaluator_;
  std::vector<float> grad_, hess_, preds_;
  std::vector< std::vector<float> > eval_preds_;
};
}  // namespace learner
}  // namespace xgboost
#endif