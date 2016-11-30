//
// Copyright (c) Microsoft. All rights reserved.
//
// Licensed under custom Microsoft Research License Terms for
// 1-bit Stochastic Gradient Descent.
// See LICENSE.md file in the project root for full license information.
//

#pragma  once

#include <vector>
#include "CNTKLibrary.h"
#include "DistributedLearnerBase.h"
#include <numeric>

namespace CNTK
{
    ///
    /// Block Momentum Trainer.
    ///
    class BlockMomentumDistributedLearner : public DistributedLearnerBase
    {
        template<class T> using Matrix = Microsoft::MSR::CNTK::Matrix<T>;

    public:
        BlockMomentumDistributedLearner(
            DistributedCommunicatorPtr communicator,
            const std::vector<LearnerPtr>& learners,
            size_t distributedAfterSamples,
            size_t globalModelAggregationBlockSize,
            bool useNesterovMomentum,
            bool resetSGDMomentumAfterAggregation,
            double blockLearningRate)
            : BlockMomentumDistributedLearner(
                  communicator,
                  learners,
                  distributedAfterSamples,
                  globalModelAggregationBlockSize,
                  useNesterovMomentum,
                  resetSGDMomentumAfterAggregation,
                  blockLearningRate,
                  Momentum2TimeConstant(1.0 - 1.0 / (double)communicator->Workers().size(), globalModelAggregationBlockSize))
        {}

        BlockMomentumDistributedLearner(
            DistributedCommunicatorPtr communicator,
            const std::vector<LearnerPtr>& learners,
            size_t distributedAfterSamples,
            size_t globalModelAggregationBlockSize,
            bool useNesterovMomentum,
            bool resetSGDMomentumAfterAggregation,
            double blockLearningRate,
            double blockMomentumAsTimeConstant)
            : DistributedLearnerBase(communicator, learners, distributedAfterSamples),
            m_useNesterovMomentum(useNesterovMomentum),
            m_resetSGDMomentumAfterAggregation(resetSGDMomentumAfterAggregation),
            m_blockLearningRate(blockLearningRate),
            m_blockMomentumAsTimeConstantPerWorker(blockMomentumAsTimeConstant / communicator->Workers().size()),
            m_globalModelAggregationBlockSize(globalModelAggregationBlockSize),
            m_numSamplesSeenInCurrentBlock(0),
            m_endOfDataReached(false),
            m_shutdown(false),
            m_localTotalNumSamplesSeen(0),
            m_syncPeriodPerWorker(globalModelAggregationBlockSize / communicator->Workers().size())
        {
            if (m_syncPeriodPerWorker == 0)
                InvalidArgument("Sync period is too small.");
        }

        bool Update(std::vector<std::pair<Parameter, NDArrayViewPtr>>& gradientValues, MinibatchInfo& info, size_t& totalNumberOfSampleSeen) override
        {
            bool finished = PerformDistributedUpdateIfNeeded(gradientValues, info);
            totalNumberOfSampleSeen = m_totalNumberOfSamplesSeen;

            // For block momentum the number of aggreagate/checkpoints should match, so for now we ignore the return value of local learners.
            if (!info.IsEmpty())
            {
                size_t ignored = 0;
                m_learner->Update(gradientValues, info, ignored);
            }

            return finished;
        }

        // Optionally overridable method to get checkpoint state associated with this Distributed train method
        Dictionary CreateCheckpoint() override
        {
            std::vector<NDArrayViewPtr> parameters;
            GetParameterValues(m_learner->Parameters(), parameters);

            // Always aggregate before the checkpoint
            SynchronizeAction(Action::Aggregate);
            AggregateImpl(parameters);

            // During checkpoint, other workers could be in aggregation state. Let's allow them to finish aggregation.
            Action action;
            while ((action = SynchronizeAction(Action::Checkpoint)) != Action::Checkpoint)
            {
                if (action == Action::Aggregate)
                    AggregateImpl(parameters);
                else
                    RuntimeError("Unexpected action received.");
            }

            Dictionary result;
            result[L"base"] = DistributedLearnerBase::CreateCheckpoint();
            result[L"localTotalNumSamplesSeen"] = m_localTotalNumSamplesSeen;
            return result;
        }

        void RestoreFromCheckpoint(const Dictionary& checkpoint) override
        {
            m_localTotalNumSamplesSeen = checkpoint[L"localTotalNumSamplesSeen"].Value<size_t>();
            DistributedLearnerBase::RestoreFromCheckpoint(checkpoint[L"base"].Value<Dictionary>());
        }

        ~BlockMomentumDistributedLearner()
        {
            if (!m_shutdown)
            {
                // It is better to throw from the destructor and crash in this case.
                RuntimeError(
                    "Block Momentum Learner was not properly shut down, your training loop is probably wrong, this could lead to hangs."
                    "Please have a look at the canonical training loop and follow the pattern.");
            }
        }

    private:
        // Optional override that gets called per minibatch after finishing gradient computation but before updating model parameters
        bool PerformDistributedUpdateIfNeeded(std::vector<std::pair<Parameter, NDArrayViewPtr>>& gradientValues, MinibatchInfo& info)
        {
            // If the last minibatch, set the end of data state.
            if (info.atEndOfData)
                m_endOfDataReached = true;

            m_localTotalNumSamplesSeen += info.numberOfSamples;
            m_totalNumberOfSamplesSeen += info.numberOfSamples;

            // TODO: Should be on the higher level of abstraction. Distributed trainer should not even be called during warmup phase.
            if (m_distributeAfterSamples > m_totalNumberOfSamplesSeen)
                return !m_endOfDataReached;

            std::vector<NDArrayViewPtr> parameters;
            if (!m_endOfDataReached)
            {
                m_numSamplesSeenInCurrentBlock += info.numberOfSamples;
                if (m_numSamplesSeenInCurrentBlock < m_syncPeriodPerWorker)
                    return false;

                GetParameterValues(gradientValues, parameters);
                Aggregate(parameters);
                return false;
            }

            GetParameterValues(gradientValues, parameters);
            return Shutdown(parameters);
        }

        // Before doing any work, the distributed trainer synchronizes with other trainers to
        // decide what to do next.
        // The priority of actons are:
        // 1) If any worker wants to aggregate - aggregation is done.
        // 2) If any worker wants to checkpoint and nobody wants to aggregate - checkpointing is done.
        // 3) If all want to shutdown - it means we reached the end of the data and shutdown can be done.
        // The priority above eliminate resolves situations when some of the workers run out of data
        // and other workers require checkpointing or aggregation.
        enum class Action
        {
            Aggregate,
            Checkpoint,
            Shutdown
        };

        void GetParameterValues(const std::vector<std::pair<Parameter, NDArrayViewPtr>>& gradientValues, std::vector<NDArrayViewPtr>& result)
        {
            for (auto p : gradientValues)
                result.push_back(p.first.Value());
        }

        void GetParameterValues(const std::vector<Parameter>& parameters, std::vector<NDArrayViewPtr>& result)
        {
            for (auto p : parameters)
                result.push_back(p.Value());
        }

        void Aggregate(std::vector<NDArrayViewPtr>& parameters)
        {
            // Synchronization action. Aggregate has the highest priority, so the expected result is aggregate.
            Action action = SynchronizeAction(Action::Aggregate);
            if (action != Action::Aggregate)
                LogicError("Unexpected action during aggregation.");

            AggregateImpl(parameters);
        }

        bool Shutdown(std::vector<NDArrayViewPtr>& parameters)
        {
            // During shutdown, other workers could be in checkpointing or aggregation state.
            // Finished workers should properly behave in this case.
            Action action;
            while ((action = SynchronizeAction(Action::Shutdown)) != Action::Shutdown)
            {
                switch (action)
                {
                case Action::Aggregate:
                    AggregateImpl(parameters);
                    break;
                case Action::Checkpoint:
                    // Somebody still has to call the checkpoint from the outside.
                    return false;
                default:
                    RuntimeError("Unexpected action received.");
                }
            }

            // Last synchronization
            AggregateImpl(parameters);
            m_shutdown = true;
            return true; // Make compiler happy.
        }

        Action SynchronizeAction(Action self)
        {
            assert(self == Action::Checkpoint || self == Action::Aggregate || self == Action::Shutdown);

            double data[2] = { static_cast<double>(self), static_cast<double>(m_localTotalNumSamplesSeen) };
            auto a = std::make_shared<NDArrayView>(DataType::Double, NDShape{ 2 }, &data, sizeof(double) * 2, DeviceDescriptor::CPUDevice());
            m_communicator->Concatenate(std::vector<NDArrayViewPtr> { a }, m_actionBuffer, m_communicator->Workers());
            assert(m_actionBuffer.size() == 1);

            auto buffer = m_actionBuffer.front()->DataBuffer<double>();
            auto bufferSize = m_actionBuffer.front()->Shape().TotalSize();
            auto bufferEnd = buffer + bufferSize;

            std::vector<Action> actions;
            actions.reserve(m_communicator->Workers().size());

            std::vector<size_t> localNumberOfSamples;
            localNumberOfSamples.reserve(m_communicator->Workers().size());

            for (const double* start = buffer; start != bufferEnd; start +=2)
            {
                actions.push_back(static_cast<Action>((int)*start));
                localNumberOfSamples.push_back(static_cast<size_t>(*(start + 1)));
            }
            m_totalNumberOfSamplesSeen = std::accumulate(localNumberOfSamples.begin(), localNumberOfSamples.end(), (size_t)0);

            auto aggregate = std::find_if(actions.begin(), actions.end(), [](Action c) { return c == Action::Aggregate; }) != actions.end();
            if (aggregate)
                return Action::Aggregate;

            auto checkpoint = std::find_if(actions.begin(), actions.end(), [](Action c) { return c == Action::Checkpoint; }) != actions.end();
            if (checkpoint)
                return Action::Checkpoint;

            // All neither aggregate nor checkpoint -> shutdown.
            return Action::Shutdown;
        }

        void AggregateImpl(std::vector<NDArrayViewPtr>& parameters)
        {
            if (IsResetRequired(parameters))
                Reset(parameters);

            m_numSamplesSeenInCurrentBlock = 0;

            // Let update the weights.
            if (parameters.front()->GetDataType() == DataType::Double)
                SynchronizeModel<double>(parameters);
            else if (parameters.front()->GetDataType() == DataType::Float)
                SynchronizeModel<float>(parameters);
            else
                RuntimeError("Unsupported type.");

            if (m_resetSGDMomentumAfterAggregation)
                m_learner->ResetSmoothedGradients();
        }

        Dictionary CreateCheckpointImpl(std::vector<NDArrayViewPtr>& parameters)
        {
            // During checkpoint, other workers could be in aggregation state. Let's allow them to finish aggregation.
            Action action;
            while ((action = SynchronizeAction(Action::Checkpoint)) != Action::Checkpoint)
            {
                if (action == Action::Aggregate)
                    AggregateImpl(parameters);
                else
                    RuntimeError("Unexpected action received.");
            }

            return DistributedLearnerBase::CreateCheckpoint();
        }

        bool IsResetRequired(std::vector<NDArrayViewPtr>& parameters) const
        {
            if (m_prevParameters.size() != parameters.size() ||
                m_blockLevelSmoothedGradient.size() != parameters.size())
                return true;

            for (size_t i = 0; i < parameters.size(); ++i)
            {
                if (m_prevParameters[i]->Shape() != parameters[i]->Shape() ||
                    m_prevParameters[i]->Device() != parameters[i]->Device() ||
                    m_blockLevelSmoothedGradient[i]->Shape() != parameters[i]->Shape() ||
                    m_blockLevelSmoothedGradient[i]->Device() != parameters[i]->Device())
                {
                    return true;
                }
            }
            return false;
        }

        void Reset(std::vector<NDArrayViewPtr>& parameters)
        {
            m_blockLevelSmoothedGradient.resize(parameters.size());
            m_prevParameters.resize(parameters.size());

            for (size_t i = 0; i < parameters.size(); ++i)
            {
                auto& p = parameters[i];

                if (p->GetDataType() == DataType::Double)
                    ResetBuffer<double>(i, p);
                else if (p->GetDataType() == DataType::Float)
                    ResetBuffer<float>(i, p);
                else
                    RuntimeError("Unsupported type.");
            }
        }

        template<class ElemType>
        void ResetBuffer(size_t index, const NDArrayViewPtr& p)
        {
            auto data = p->GetMatrix<ElemType>();
            if (!m_blockLevelSmoothedGradient[index])
            {
                // has not been initialized yet
                NDShape shape{ data->GetNumRows(), data->GetNumCols() };
                auto pSmoothedGrad = std::make_shared<NDArrayView>(AsDataType<ElemType>(), shape, AsDeviceDescriptor(data->GetDeviceId()));
                pSmoothedGrad->SetValue(static_cast<ElemType>(0));
                m_blockLevelSmoothedGradient[index] = pSmoothedGrad;
            }

            if (!m_prevParameters[index])
            {
                NDShape shape{ data->GetNumRows(), data->GetNumCols() };
                NDArrayViewPtr newValue = std::make_shared<NDArrayView>(AsDataType<ElemType>(), shape, AsDeviceDescriptor(data->GetDeviceId()));
                std::shared_ptr<Matrix<ElemType>> newData = newValue->GetWritableMatrix<ElemType>();
                newData->SetValue(*data);
                m_prevParameters[index] = newValue;
            }
            else
            {
                m_prevParameters[index]->GetWritableMatrix<ElemType>()->SetValue(*data);
            }
        }

        template<class ElemType>
        void SynchronizeModel(const std::vector<NDArrayViewPtr>& gradientValues)
        {
            ElemType blockMomentum = (ElemType)TimeConstant2Momentum(m_blockMomentumAsTimeConstantPerWorker, m_syncPeriodPerWorker);

            // 1. Let's aggregate weights
            std::vector<std::shared_ptr<Matrix<ElemType>>> aggregatedWeights;
            std::vector<NDArrayViewPtr> aggregatedWeightsPrepared;
            for (size_t i = 0; i < gradientValues.size(); ++i)
            {
                // Get current model
                Matrix<ElemType>& previousWeight = *m_prevParameters[i]->GetWritableMatrix<ElemType>();                  // prev model value
                Matrix<ElemType>& currentWeight = *gradientValues[i]->GetWritableMatrix<ElemType>();

                // Subtract it from the previous model
                auto blockGrad = std::make_shared<Matrix<ElemType>>(previousWeight, CPUDEVICE);
                *blockGrad -= currentWeight;                                              // matW becomes local block gradient (of one worker)

                aggregatedWeights.push_back(blockGrad);
                NDShape shape{ blockGrad->GetNumElements() };
                auto data = MakeSharedObject<NDArrayView>(AsDataType<ElemType>(), shape, blockGrad->Data(), blockGrad->GetNumElements() * sizeof(ElemType), AsDeviceDescriptor(blockGrad->GetDeviceId()));
                aggregatedWeightsPrepared.push_back(data);
            }

            // Send block gradient over MPI nodes.
            m_communicator->AggregateInPlace(aggregatedWeightsPrepared, m_communicator->Workers());

            // 2. Let's update the model
            for (size_t i = 0; i < gradientValues.size(); ++i)
            {
                // 2 block gradient aggregation
                // 2.1. get current model
                Matrix<ElemType>& previousWeight = *m_prevParameters[i]->GetWritableMatrix<ElemType>();                  // prev model value
                Matrix<ElemType>& currentWeight = *gradientValues[i]->GetWritableMatrix<ElemType>();
                auto blockGrad = aggregatedWeights[i];
                // 2.2. model update 
                {
                    Matrix<ElemType>& sg = *m_blockLevelSmoothedGradient[i]->GetWritableMatrix<ElemType>();       // smoothed gradient
                    blockGrad->TransferToDeviceIfNotThere(sg.GetDeviceId());
                    // 2.2.1 update block level smoothed gradient; 
                    // This is essentially a first-order infinite impulse response (IIR) filter with the gain (1 - blockMomentum)*m_blockLearningRate:
                    // smoothedGradient(t)=blockMomentum * smoothedGradients(t-1) + (1 - blockMomentum)*m_blockLearningRate*blockGrad(t)
                    Matrix<ElemType>::ScaleAndAdd((ElemType)((1 - blockMomentum)*m_blockLearningRate), *blockGrad, (ElemType)blockMomentum, sg);
                    // 2.2.2 update parameters; 
                    currentWeight.SetValue(previousWeight);
                    currentWeight -= sg;
                    // 2.2.3 Nesterov Momentum 
                    // A Nesterov momentum here is to do a partial weight update before calculating the gradient, i.e., 
                    // (step 1) w(t) <-- w(t) - \eta* v(t) 
                    // (step 2) g(t+1) <-- forwardbackward on minibatches with initial model as w(t)
                    // (step 3) v(t+1) <-- \eta*v(t) + (1-\eta)*learningRate*g(t+1)
                    // (step 4) w(t+1) <-- w(t)-v(t)
                    // (step 5) t      <-- t+1
                    // without step 1, this becomes stanard momentum
                    if (m_useNesterovMomentum)
                    {
                        Matrix<ElemType>::ScaleAndAdd((ElemType)-blockMomentum, sg, currentWeight);
                    }
                    // 2.2.4 update bookkeeping
                    previousWeight.SetValue(currentWeight);
                }
            }
        }

        static double TimeConstant2Momentum(double timeConstant, size_t syncPeroid)
        {
            return exp(-((double)syncPeroid) / timeConstant);
        }

        static double Momentum2TimeConstant(double bm, size_t syncPeroid)
        {
            if (bm >= 1.0 || bm < 0.0)
            {
                InvalidArgument("Unexpected block momentum (%.2f). Block momentum should be in the range of [0,1)\n", bm);
            }
            return -(double)syncPeroid / log(bm);
        }

        const bool m_resetSGDMomentumAfterAggregation;
        const bool m_useNesterovMomentum;
        const double m_blockLearningRate;
        const double m_blockMomentumAsTimeConstantPerWorker;

        const size_t m_syncPeriodPerWorker;
        const size_t m_globalModelAggregationBlockSize;
        size_t m_numSamplesSeenInCurrentBlock;
        size_t m_localTotalNumSamplesSeen;

        // parameters at the last model aggregation point
        std::vector<NDArrayViewPtr> m_prevParameters;
        std::vector<NDArrayViewPtr> m_blockLevelSmoothedGradient;
        std::vector<NDArrayViewPtr> m_actionBuffer;

        bool m_endOfDataReached;
        bool m_shutdown;

        DISABLE_COPY_AND_MOVE(BlockMomentumDistributedLearner);
     };
}
