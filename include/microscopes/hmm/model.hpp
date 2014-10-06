#pragma once

#include <distributions/special.hpp>
#include <distributions/random.hpp>
#include <distributions/random_fwd.hpp>

#include <vector>
#include <map>

namespace microscopes{
namespace hmm{
  

  distributions::rng_t rng;

  // A class for a vector of vectors, useful for representing pretty much everything we need for the beam sampler.
  // For instance, time series data can be stored as a vector of vectors, where each vector is one time series.
  // The transition matrix can also be stored as a vector of vectors, where each transition probability is one vector.

  template <typename T>
  class meta_vector {
  public:

    meta_vector() : data_() {}

    meta_vector(size_t size) : data_(size) {}

    meta_vector(size_t size_1, size_t size_2) : data_(size_1) {
      for (int i = 0; i < size_1; i++) {
        data_[i] = std::vector<T>(size_2);
      }
    }

    meta_vector(std::vector<size_t> size) : data_(size.size()) {
      for (int i = 0; i < data_.size(); i++) {
        data_[i] = std::vector<T>(size[i]);
      }
    }

    std::vector<T>& operator[](size_t i) {
      return data_[i];
    }

    void push_back(std::vector<T> vec) {
      data_.push_back(vec);
    }

    std::vector<size_t> size() {
      std::vector<size_t> sizes(data_.size());
      for (std::vector<T> vec: data_) {
        sizes.push_back(vec.size());
      }
      return sizes;
    }

    T sum(size_t i) { // useful for resampling m, the number of tables serving a dish
      T result = (T)0;
      for(T t: data_[i]) {
        result += t;
      }
      return result;
    }
  protected:
    std::vector<std::vector<T> > data_;
  };

// Implementation of the beam sampler for the HDP-HMM, following van Gael 2008
  template <int N>
  class hmm {
  public:
    hmm(float gamma, float alpha0, float *H, meta_vector<size_t> data):
      gamma_(gamma),
      alpha0_(alpha0),
      H_(H),
      data_(data),
      u_(data.size()),
      m_(1,1),
      counts_(1,1),
      pi_(1,2),
      phi_(1,N),
      beta_(2),
      memoized_log_stirling_(),
      K(1)
    {
    }

    void sample_beam() {
      sample_u();
      sample_s();
      sample_pi();
      sample_phi();
      sample_beta();
    }
  protected:
    
    // parameters

    // these three all have the same shape as the data
    const meta_vector<size_t> data_; // XXX: For now, the observation type is just a vector of vectors of ints. Later we can switch over to using recarrays
    meta_vector<size_t> s_; // the state sequence
    meta_vector<float> u_; // the slice sampling parameter for each time step in the series

    // these three all have the same shape as the transition matrix, approximately
    meta_vector<size_t> m_; // auxilliary variable necessary for sampling beta. Size K x K.
    meta_vector<size_t> counts_; // the count of how many times a transition occurs between states. Size K x K.
    meta_vector<float> pi_; // the observed portion of the infinite transition matrix. Size K x K+1.

    meta_vector<float> phi_; // the emission matrix. Size K x N.

    // shape is the number of states currently instantiated
    std::vector<float> beta_; // the stick lengths for the top-level DP draw. Size K+1.

    // hyperparameters
    const float gamma_;
    const float alpha0_;
    const float H_[N]; // hyperparameters for a Dirichlet prior over observations. Will generalize this to other observation models later.

    // helper fields
    std::map<size_t, std::vector<float> > memoized_log_stirling_; // memoize computation of log stirling numbers for speed when sampling m
    // Over all instantiated states, the maximum value of the part of pi_k that belongs to the "unseen" states. 
    //Should be smaller than the least value of the auxiliary variable, so all possible states visited by the beam sampler are instantiated
    float max_pi; 
    size_t K;

    // sampling functions. later we can integrate these into microscopes::kernels where appropriate.
    void sample_s() {
      std::vector<size_t> sizes = data_.size();
      counts_ = meta_vector<size_t>(pi_.size().size()); // clear counts
      for (int i = 0; i < sizes.size(); i++) {
        // Forward-filter
        meta_vector<float> probs = meta_vector<float>(sizes[i]);
        for (int t = 0; t < sizes[i]; t++) {
          probs[t] = std::vector<float>(K);
          float total_prob = 0.0;
          for (int k = 0; k < K; k++) {
            if (t == 0) {
              probs[t][k] = phi_[k][data_[i][t]] * (u_[i][t] < pi_[0][k] ? 1.0 : 0.0);
            }
            else {
              probs[t][k] = 0.0;
              for (int l = 0; l < K; l++) {
                if (u_[i][t] < pi_[l][k]) {
                  probs[t][k] += probs[t-1][l];
                }
              }
              probs[t][k] *= phi_[k][data_[i][t]];
            }
            total_prob += probs[t][k];
          }
          for (int k = 0; k < K; k++) { // normalize to prevent numerical underflow
            probs[t][k] /= total_prob;
          }
        }

        // Backwards-sample
        s_[i][sizes[i]-1] = distributions::sample_from_likelihoods(rng, probs[sizes[i]-1]);
        for (int t = sizes[i]-1; t > 0; t--) {
          for (int k = 0; k < K; k++) {
            if (u_[i][t] >= pi_[k][s_[i][t]]) {
              probs[t-1][k] = 0;
            }
          }
          s_[i][t-1] = distributions::sample_from_likelihoods(rng, probs[t-1]);
          // Update counts

        }
      }
    }

    void sample_u() {
      size_t prev_state;
      std::uniform_real_distribution<float> sampler (0.0, 1.0);
      std::vector<size_t> sizes = u_.size();
      float min_u = 1.0; // used to figure out where to truncate sampling of pi
      for (int i = 0; i < sizes.size(); i++) {
        for(int j = 0; j < sizes[i]; j++) {
          if (j == 0) {
            prev_state = 0;
          } else {
            prev_state = s_[i][j-1];
          }
          u_[i][j] = sampler(rng) / (pi_[prev_state][s_[i][j]]); // scale the uniform sample to be between 0 and pi_{s_{t-1}s_t}
          min_u = min_u < u_[i][j] ? min_u : u_[i][j];
        }
      }

      // If necessary, break the pi stick some more
      while (max_pi > min_u) {
        // Add new state
        pi_.push_back(std::vector<float>(K+1));
        sample_pi_row(K);

        // Break beta stick
        float bu = beta_[K];
        float bk = distributions::sample_beta(rng, 1.0, gamma_);
        beta_[K] = bu * bk;
        beta_.push_back(bu * (1-bk));

        // Add new transition to each state
        max_pi = 0.0;
        for (int i = 0; i < K+1; i++) {
          float pu = pi_[i][K];
          float pk = distributions::sample_beta(rng, alpha0_ * beta_[K], alpha0_ * beta_[K+1]);
          pi_[i][K] = pu * pk;
          pi_[i].push_back(pu * (1-pk));
          max_pi = max_pi > pi_[i][K]   ? max_pi : pi_[i][K];
          max_pi = max_pi > pi_[i][K+1] ? max_pi : pi_[i][K+1];
        }
        K++;
      }
    }

    void sample_pi() {
      max_pi = 0.0;
      for (int i = 0; i < K; i++) {
        sample_pi_row(i);
      }
    }

    void sample_pi_row(size_t i) {
        float new_pi[K+1];
        float alphas[K+1];
        for (int k = 0; k < K; k++) {
          alphas[k] = counts_[i][k] + alpha0_ * beta_[k];
        }
        alphas[K] = alpha0_ * beta_[K];
        distributions::sample_dirichlet(rng, K+1, alphas, new_pi);
        for (int j = 0; j < K+1; j++) {
          pi_[i][j] = new_pi[i];
        }
        max_pi = max_pi > new_pi[K] ? max_pi : new_pi[K];
    }

    void sample_phi() {

    }

    void sample_m() {
      for (int i = 0; i < K; i++) {
        for (int j = 0; j < K; j++) {
          size_t n_ij = counts_[i][j];
          if (!memoized_log_stirling_.count(n_ij)) {
            memoized_log_stirling_[n_ij] = distributions::log_stirling1_row(n_ij);
          }

          std::vector<float> stirling_row = memoized_log_stirling_[n_ij];

          std::vector<float> scores(n_ij);
          for (int m = 0; m < n_ij; m++) {
            scores[m] = stirling_row[m+1] + (m+1) * ( log( alpha0_ ) + log( beta_[j] ) );
          }
          m_[i][j] = distributions::sample_from_scores_overwrite(rng, scores) + 1;
        }
      }
    }

    void sample_beta() {
      sample_m();
      float alphas[K+1];
      float new_beta[K+1];
      for (int k = 0; k < K; k++) {
        alphas[k] = m_.sum(k);
      }
      alphas[K] = gamma_;
      distributions::sample_dirichlet(rng, K+1, alphas, new_beta);
      for (int k = 0; k <= K; k++) {
        beta_[k] = new_beta[k];
      }
    }
  };

} // namespace hmm
} // namespace microscopes