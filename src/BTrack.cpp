//=======================================================================
/** @file BTrack.cpp
 *  @brief BTrack - a real-time beat tracker
 *  @author Adam Stark
 *  @copyright Copyright (C) 2008-2014  Queen Mary University of London
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
//=======================================================================

#include <cmath>
#include <algorithm>
#include <numeric>
#include "BTrack.h"
#include "samplerate.h"
#include <iostream>

//=======================================================================
BTrack::BTrack()
 :  odf (512, 1024, ComplexSpectralDifferenceHWR, HanningWindow)
{
    initialise (512, 1024);
}

//=======================================================================
BTrack::BTrack (int hopSize_)
 :  odf (hopSize_, 2 * hopSize_, ComplexSpectralDifferenceHWR, HanningWindow)
{	
    initialise (hopSize_, 2 * hopSize_);
}

//=======================================================================
BTrack::BTrack (int hopSize_, int frameSize_)
 : odf (hopSize_, frameSize_, ComplexSpectralDifferenceHWR, HanningWindow)
{
    initialise (hopSize_, frameSize_);
}

//=======================================================================
BTrack::~BTrack()
{
#ifdef USE_FFTW
    // destroy fft plan
    fftw_destroy_plan (acfForwardFFT);
    fftw_destroy_plan (acfBackwardFFT);
    fftw_free (complexIn);
    fftw_free (complexOut);
#endif
    
#ifdef USE_KISS_FFT
    free (cfgForwards);
    free (cfgBackwards);
    delete [] fftIn;
    delete [] fftOut;
#endif
}

//=======================================================================
double BTrack::getBeatTimeInSeconds (long frameNumber, int hopSize, int fs)
{
    double hop = (double) hopSize;
    double samplingFrequency = (double) fs;
    double frameNum = (double) frameNumber;
    
    return ((hop / samplingFrequency) * frameNum);
}

//=======================================================================
void BTrack::initialise (int hopSize_, int frameSize_)
{
    // set vector sizes
    resampledOnsetDF.resize (512);
    acf.resize (512);
    weightingVector.resize (128);
    combFilterBankOutput.resize (128);
    tempoObservationVector.resize (41);
    delta.resize (41);
    prevDelta.resize (41);
    prevDeltaFixed.resize (41);
    
    double rayleighParameter = 43;
	double pi = 3.14159265;
	
	
	// initialise parameters
	tightness = 5;
	alpha = 0.9;
	estimatedTempo = 120.0;
	tempoToLagFactor = 60. * 44100. / 512.;
	
	m0 = 10;
	beatCounter = -1;
	
	beatDueInFrame = false;
	

	// create rayleigh weighting vector
	for (int n = 0; n < 128; n++)
        weightingVector[n] = ((double) n / pow (rayleighParameter, 2)) * exp((-1 * pow((double) - n, 2)) / (2 * pow (rayleighParameter, 2)));
	
    // initialise prevDelta
    std::fill (prevDelta.begin(), prevDelta.end(), 1);
    
	double t_mu = 41/2;
	double m_sig;
	double x;
	// create tempo transition matrix
	m_sig = 41/8;
	for (int i = 0;i < 41;i++)
	{
		for (int j = 0;j < 41;j++)
		{
			x = j+1;
			t_mu = i+1;
			tempoTransitionMatrix[i][j] = (1 / (m_sig * sqrt(2*pi))) * exp( (-1*pow((x-t_mu),2)) / (2*pow(m_sig,2)) );
		}
	}
	
	// tempo is not fixed
	tempoFixed = false;
    
    // initialise latest cumulative score value
    // in case it is requested before any processing takes place
    latestCumulativeScoreValue = 0;
    
    // initialise algorithm given the hopsize
    setHopSize (hopSize_);
    
    // Set up FFT for calculating the auto-correlation function
    FFTLengthForACFCalculation = 1024;
    
#ifdef USE_FFTW
    complexIn = (fftw_complex*) fftw_malloc (sizeof(fftw_complex) * FFTLengthForACFCalculation);		// complex array to hold fft data
    complexOut = (fftw_complex*) fftw_malloc (sizeof(fftw_complex) * FFTLengthForACFCalculation);	// complex array to hold fft data
    
    acfForwardFFT = fftw_plan_dft_1d (FFTLengthForACFCalculation, complexIn, complexOut, FFTW_FORWARD, FFTW_ESTIMATE);	// FFT plan initialisation
    acfBackwardFFT = fftw_plan_dft_1d (FFTLengthForACFCalculation, complexOut, complexIn, FFTW_BACKWARD, FFTW_ESTIMATE);	// FFT plan initialisation
#endif
    
#ifdef USE_KISS_FFT
    fftIn = new kiss_fft_cpx[FFTLengthForACFCalculation];
    fftOut = new kiss_fft_cpx[FFTLengthForACFCalculation];
    cfgForwards = kiss_fft_alloc (FFTLengthForACFCalculation, 0, 0, 0);
    cfgBackwards = kiss_fft_alloc (FFTLengthForACFCalculation, 1, 0, 0);
#endif
}

//=======================================================================
void BTrack::setHopSize (int hopSize_)
{	
	hopSize = hopSize_;
	onsetDFBufferSize = (512 * 512) / hopSize;		// calculate df buffer size
	beatPeriod = round(60/((((double) hopSize)/44100) * 120.));

    // set size of onset detection function buffer
    onsetDF.resize (onsetDFBufferSize);
    
    // set size of cumulative score buffer
    cumulativeScore.resize (onsetDFBufferSize);
	
	// initialise df_buffer to zeros
	for (int i = 0; i < onsetDFBufferSize; i++)
	{
		onsetDF[i] = 0;
		cumulativeScore[i] = 0;
		
		if ((i %  ((int) round(beatPeriod))) == 0)
		{
			onsetDF[i] = 1;
		}
	}
}

//=======================================================================
void BTrack::updateHopAndFrameSize (int hopSize_, int frameSize_)
{
    // update the onset detection function object
    odf.initialise (hopSize_, frameSize_);
    
    // update the hop size being used by the beat tracker
    setHopSize (hopSize_);
}

//=======================================================================
bool BTrack::beatDueInCurrentFrame()
{
    return beatDueInFrame;
}

//=======================================================================
double BTrack::getCurrentTempoEstimate()
{
    return estimatedTempo;
}

//=======================================================================
int BTrack::getHopSize()
{
    return hopSize;
}

//=======================================================================
double BTrack::getLatestCumulativeScoreValue()
{
    return latestCumulativeScoreValue;
}

//=======================================================================
void BTrack::processAudioFrame (double* frame)
{
    // calculate the onset detection function sample for the frame
    double sample = odf.calculateOnsetDetectionFunctionSample (frame);
    
    // process the new onset detection function sample in the beat tracking algorithm
    processOnsetDetectionFunctionSample (sample);
}

//=======================================================================
void BTrack::processOnsetDetectionFunctionSample (double newSample)
{
    // we need to ensure that the onset
    // detection function sample is positive
    newSample = fabs (newSample);
    
    // add a tiny constant to the sample to stop it from ever going
    // to zero. this is to avoid problems further down the line
    newSample = newSample + 0.0001;
    
	m0--;
	beatCounter--;
	beatDueInFrame = false;
		
	// add new sample at the end
    onsetDF.addSampleToEnd (newSample);
	
	// update cumulative score
	updateCumulativeScore (newSample);
	
	// if we are halfway between beats, predict a beat
	if (m0 == 0)
        predictBeat();
	
	// if we are at a beat
	if (beatCounter == 0)
	{
		beatDueInFrame = true;	// indicate a beat should be output
		
		// recalculate the tempo
		resampleOnsetDetectionFunction();
		calculateTempo();
	}
}

//=======================================================================
void BTrack::setTempo (double tempo)
{
	/////////// TEMPO INDICATION RESET //////////////////
	
	// firstly make sure tempo is between 80 and 160 bpm..
	while (tempo > 160)
        tempo = tempo / 2;
	
	while (tempo < 80)
        tempo = tempo * 2;
		
	// convert tempo from bpm value to integer index of tempo probability 
	int tempo_index = (int) round((tempo - 80)/2);
	
    // now set previous tempo observations to zero and set desired tempo index to 1
    std::fill (prevDelta.begin(), prevDelta.end(), 0);
	prevDelta[tempo_index] = 1;
	
	/////////// CUMULATIVE SCORE ARTIFICAL TEMPO UPDATE //////////////////
	
	// calculate new beat period
	int newBeatPeriod = (int) round (60 / ((((double) hopSize) / 44100) * tempo));
	
	int k = 1;
    
    // initialise onset detection function with delta functions spaced
    // at the new beat period
	for (int i = onsetDFBufferSize - 1; i >= 0; i--)
	{
		if (k == 1)
		{
			cumulativeScore[i] = 150;
			onsetDF[i] = 150;
		}
		else
		{
			cumulativeScore[i] = 10;
			onsetDF[i] = 10;
		}
		
		k++;
		
		if (k > newBeatPeriod)
		{
			k = 1;
		}
	}
	
	/////////// INDICATE THAT THIS IS A BEAT //////////////////
	
	// beat is now
	beatCounter = 0;
	
	// offbeat is half of new beat period away
	m0 = (int) round (((double) newBeatPeriod) / 2);
}

//=======================================================================
void BTrack::fixTempo (double tempo)
{	
	// firstly make sure tempo is between 80 and 160 bpm..
	while (tempo > 160)
        tempo = tempo / 2;
	
	while (tempo < 80)
        tempo = tempo * 2;
	
	// convert tempo from bpm value to integer index of tempo probability 
	int tempoIndex = (int) round((tempo - 80) / 2);
	
	// now set previous fixed previous tempo observation values to zero
	for (int i=0;i < 41;i++)
	{
		prevDeltaFixed[i] = 0;
	}
	
	// set desired tempo index to 1
	prevDeltaFixed[tempoIndex] = 1;
		
	// set the tempo fix flag
	tempoFixed = true;
}

//=======================================================================
void BTrack::doNotFixTempo()
{	
	// set the tempo fix flag
	tempoFixed = false;
}

//=======================================================================
void BTrack::resampleOnsetDetectionFunction()
{
	float output[512];
    float input[onsetDFBufferSize];
    
    for (int i = 0;i < onsetDFBufferSize;i++)
        input[i] = (float) onsetDF[i];
        
    double ratio = 512.0 / ((double) onsetDFBufferSize);
    int bufferLength = onsetDFBufferSize;
    int outputLength = 512;
    
    SRC_DATA src_data;
    src_data.data_in = input;
    src_data.input_frames = bufferLength;
    src_data.src_ratio = ratio;
    src_data.data_out = output;
    src_data.output_frames = outputLength;
    
    src_simple (&src_data, SRC_SINC_BEST_QUALITY, 1);
            
    for (int i = 0; i < outputLength; i++)
        resampledOnsetDF[i] = (double) src_data.data_out[i];
}

//=======================================================================
void BTrack::calculateTempo()
{
	// adaptive threshold on input
	adaptiveThreshold (resampledOnsetDF);
		
	// calculate auto-correlation function of detection function
	calculateBalancedACF (resampledOnsetDF);
	
	// calculate output of comb filterbank
	calculateOutputOfCombFilterBank();
	
	// adaptive threshold on rcf
	adaptiveThreshold (combFilterBankOutput);

	// calculate tempo observation vector from beat period observation vector
	for (int i = 0; i < 41; i++)
	{
		int tempoIndex1 = (int) round (tempoToLagFactor / ((double) ((2*i)+80)));
		int tempoIndex2 = (int) round (tempoToLagFactor / ((double) ((4*i)+160)));
		tempoObservationVector[i] = combFilterBankOutput[tempoIndex1 - 1] + combFilterBankOutput[tempoIndex2 - 1];
	}
	
	// if tempo is fixed then always use a fixed set of tempi as the previous observation probability function
	if (tempoFixed)
	{
		for (int k = 0; k < 41; k++)
            prevDelta[k] = prevDeltaFixed[k];
	}
		
	for (int j = 0; j < 41; j++)
	{
		double maxValue = -1;
        
		for (int i = 0; i < 41; i++)
		{
			double currentValue = prevDelta[i] * tempoTransitionMatrix[i][j];
			
			if (currentValue > maxValue)
                maxValue = currentValue;
		}
		
		delta[j] = maxValue * tempoObservationVector[j];
	}
	
	normaliseVector (delta);
	
	double maxIndex = -1;
	double maxValue = -1;
	
	for (int j = 0; j < 41; j++)
	{
		if (delta[j] > maxValue)
		{
			maxValue = delta[j];
			maxIndex = j;
		}
		
		prevDelta[j] = delta[j];
	}
	
	beatPeriod = round ((60.0 * 44100.0) / (((2 * maxIndex) + 80) * ((double) hopSize)));
	
	if (beatPeriod > 0)
        estimatedTempo = 60.0/((((double) hopSize) / 44100.0) * beatPeriod);
}

//=======================================================================
void BTrack::adaptiveThreshold (std::vector<double>& x)
{
    int N = static_cast<int> (x.size());
	double threshold[N];
	
	int p_post = 7;
	int p_pre = 8;
	
	int t = std::min (N, p_post);	// what is smaller, p_post or df size. This is to avoid accessing outside of arrays
	
	// find threshold for first 't' samples, where a full average cannot be computed yet 
	for (int i = 0; i <= t; i++)
	{	
		int k = std::min ((i + p_pre), N);
		threshold[i] = calculateMeanOfVector (x, 1, k);
	}
    
	// find threshold for bulk of samples across a moving average from [i-p_pre,i+p_post]
	for (int i = t + 1; i < N - p_post; i++)
	{
		threshold[i] = calculateMeanOfVector (x, i - p_pre, i + p_post);
	}
    
	// for last few samples calculate threshold, again, not enough samples to do as above
	for (int i = N - p_post; i < N; i++)
	{
		int k = std::max ((i - p_post), 1);
		threshold[i] = calculateMeanOfVector (x, k, N);
	}
	
	// subtract the threshold from the detection function and check that it is not less than 0
	for (int i = 0; i < N; i++)
	{
		x[i] = x[i] - threshold[i];
        
		if (x[i] < 0)
            x[i] = 0;
	}
}

//=======================================================================
void BTrack::calculateOutputOfCombFilterBank()
{
    std::fill (combFilterBankOutput.begin(), combFilterBankOutput.end(), 0.0);
	int numCombElements = 4;
	
	for (int i = 2; i <= 127; i++) // max beat period
	{
		for (int a = 1; a <= numCombElements; a++) // number of comb elements
		{
			for (int b = 1 - a; b <= a - 1; b++) // general state using normalisation of comb elements
			{
				combFilterBankOutput[i-1] += (acf[(a * i + b) - 1] * weightingVector[i - 1]) / (2 * a - 1);	// calculate value for comb filter row
			}
		}
	}
}

//=======================================================================
void BTrack::calculateBalancedACF (std::vector<double>& onsetDetectionFunction)
{
    int onsetDetectionFunctionLength = 512;
    
#ifdef USE_FFTW
    // copy into complex array and zero pad
    for (int i = 0;i < FFTLengthForACFCalculation;i++)
    {
        if (i < onsetDetectionFunctionLength)
        {
            complexIn[i][0] = onsetDetectionFunction[i];
            complexIn[i][1] = 0.0;
        }
        else
        {
            complexIn[i][0] = 0.0;
            complexIn[i][1] = 0.0;
        }
    }
    
    // perform the fft
    fftw_execute (acfForwardFFT);
    
    // multiply by complex conjugate
    for (int i = 0;i < FFTLengthForACFCalculation;i++)
    {
        complexOut[i][0] = complexOut[i][0]*complexOut[i][0] + complexOut[i][1]*complexOut[i][1];
        complexOut[i][1] = 0.0;
    }
    
    // perform the ifft
    fftw_execute (acfBackwardFFT);
    
#endif
    
#ifdef USE_KISS_FFT
    // copy into complex array and zero pad
    for (int i = 0;i < FFTLengthForACFCalculation;i++)
    {
        if (i < onsetDetectionFunctionLength)
        {
            fftIn[i].r = onsetDetectionFunction[i];
            fftIn[i].i = 0.0;
        }
        else
        {
            fftIn[i].r = 0.0;
            fftIn[i].i = 0.0;
        }
    }
    
    // execute kiss fft
    kiss_fft (cfgForwards, fftIn, fftOut);
    
    // multiply by complex conjugate
    for (int i = 0;i < FFTLengthForACFCalculation;i++)
    {
        fftOut[i].r = fftOut[i].r * fftOut[i].r + fftOut[i].i * fftOut[i].i;
        fftOut[i].i = 0.0;
    }
    
    // perform the ifft
    kiss_fft (cfgBackwards, fftOut, fftIn);
    
#endif
    
    double lag = 512;
    
    for (int i = 0; i < 512; i++)
    {
#ifdef USE_FFTW
        // calculate absolute value of result
        double absValue = sqrt (complexIn[i][0]*complexIn[i][0] + complexIn[i][1]*complexIn[i][1]);
#endif
        
#ifdef USE_KISS_FFT
        // calculate absolute value of result
        double absValue = sqrt (fftIn[i].r * fftIn[i].r + fftIn[i].i * fftIn[i].i);
#endif
        // divide by inverse lad to deal with scale bias towards small lags
        acf[i] = absValue / lag;
        
        // this division by 1024 is technically unnecessary but it ensures the algorithm produces
        // exactly the same ACF output as the old time domain implementation. The time difference is
        // minimal so I decided to keep it
        acf[i] = acf[i] / 1024.;
        
        lag = lag - 1.;
    }
}

//=======================================================================
double BTrack::calculateMeanOfVector (std::vector<double>& vector, int startIndex, int endIndex)
{
    int length = endIndex - startIndex;
    double sum = std::accumulate (vector.begin() + startIndex, vector.begin() + endIndex, 0.0);

    if (length > 0)
        return sum / static_cast<double> (length);	// average and return
    else
        return 0;
}

//=======================================================================
void BTrack::normaliseVector (std::vector<double>& vector)
{
    double sum = std::accumulate (vector.begin(), vector.end(), 0.0);
	
	if (sum > 0)
    {
        for (int i = 0; i < vector.size(); i++)
            vector[i] = vector[i] / sum;
    }
}

//=======================================================================
void BTrack::updateCumulativeScore (double onsetDetectionFunctionSample)
{
	int windowStart = onsetDFBufferSize - round (2. * beatPeriod);
	int windowEnd = onsetDFBufferSize - round (beatPeriod / 2.);
	int windowSize = windowEnd - windowStart + 1;
	
	double w1[windowSize];
	double v = -2. * beatPeriod;
	
	// create window
	for (int i = 0; i < windowSize; i++)
	{
        double a = tightness * log (-v / beatPeriod);
		w1[i] = exp ((-1. * a * a) / 2.);
		v = v + 1.;
	}	
	
	// calculate new cumulative score value
	double maxValue = 0;
	int n = 0;
	for (int i = windowStart; i <= windowEnd; i++)
	{
        double weightedCumulativeScore = cumulativeScore[i] * w1[n];
		
        if (weightedCumulativeScore > maxValue)
            maxValue = weightedCumulativeScore;
        
		n++;
	}
	
    latestCumulativeScoreValue = ((1 - alpha) * onsetDetectionFunctionSample) + (alpha * maxValue);
    cumulativeScore.addSampleToEnd (latestCumulativeScoreValue);
}

//=======================================================================
void BTrack::predictBeat()
{	 
	int beatExpectationWindowSize = (int) beatPeriod;
	double futureCumulativeScore[onsetDFBufferSize + beatExpectationWindowSize];
	double beatExpectationWindow[beatExpectationWindowSize];
    
	// copy cumulativeScore to first part of futureCumulativeScore
	for (int i = 0;i < onsetDFBufferSize;i++)
        futureCumulativeScore[i] = cumulativeScore[i];
	
	// Create a beat expectation window for predicting future beats from the "future" of the cumulative score.
    // We are making this beat prediction at the midpoint between beats, and so we make a Gaussian
    // weighting centred on the most likely beat position (half a beat period into the future)
    // This is W2 in Adam Stark's PhD thesis, equation 3.6, page 62
    
	double v = 1;
	for (int i = 0; i < beatExpectationWindowSize; i++)
	{
		beatExpectationWindow[i] = exp((-1 * pow ((v - (beatPeriod / 2)), 2))   /  (2 * pow (beatPeriod / 2, 2)));
		v++;
	}
	
	// Create window for "synthesizing" the cumulative score into the future
    // It is a log-Gaussian transition weighting running from from 2 beat periods
    // in the past to half a beat period in the past. It favours the time exactly
    // one beat period in the past
    // This is W1 in Adam Stark's PhD thesis, equation 3.2, page 60
    
	v = -2 * beatPeriod;
	int startIndex = onsetDFBufferSize - round (2 * beatPeriod);
	int endIndex = onsetDFBufferSize - round (beatPeriod / 2);
	int pastWindowSize = endIndex - startIndex + 1;
	double logGaussianTransitionWeighting[pastWindowSize];

	for (int i = 0; i < pastWindowSize; i++)
	{
		logGaussianTransitionWeighting[i] = exp((-1 * pow (tightness * log (-v / beatPeriod), 2) ) / 2);
		v = v + 1;
	}

	// Calculate the future cumulative score, using the log Gaussian transition weighting
    
	for (int i = onsetDFBufferSize; i < (onsetDFBufferSize + beatExpectationWindowSize); i++)
	{
		startIndex = i - round (2 * beatPeriod);
		endIndex = i - round (beatPeriod / 2);
		
		double maxValue = 0;
		int n = 0;
		for (int k = startIndex; k <= endIndex; k++)
		{
			double weightedCumulativeScore = futureCumulativeScore[k] * logGaussianTransitionWeighting[n];
			
			if (weightedCumulativeScore > maxValue)
                maxValue = weightedCumulativeScore;

			n++;
		}
		
		futureCumulativeScore[i] = maxValue;
	}
	
	// Predict the next beat, finding the maximum point of the future cumulative score
    // over the next beat, after being weighted by the beat expectation window
    
	double maxValue = 0;
	int n = 0;
	
	for (int i = onsetDFBufferSize; i < (onsetDFBufferSize + beatExpectationWindowSize); i++)
	{
		double weightedCumulativeScore = futureCumulativeScore[i] * beatExpectationWindow[n];
		
		if (weightedCumulativeScore > maxValue)
		{
			maxValue = weightedCumulativeScore;
			beatCounter = n;
		}	
		
		n++;
	}
		
	// set next prediction time
	m0 = beatCounter + round (beatPeriod / 2);
}
