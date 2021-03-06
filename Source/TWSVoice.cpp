/**
 * This file contains all the DSP.
 *
 * Warning: The DSP isn't very good!
 */

#include "TWSVoice.h"
#include "PluginProcessor.h"

TWSVoice::TWSVoice(TuningworkbenchsynthAudioProcessor *i) : p(i) {
    // http://www.cs.cmu.edu/~music/icm-online/readings/panlaws/
    // FIXME - make this more efficient by using a lookup table obvs
    for( int i=0; i<N_PAN; ++i )
    {
        double panAngle = 1.0 * i / (N_PAN - 1) * MathConstants<double>::pi / 2.0;
        auto piby2 = MathConstants<double>::pi / 2.0;
        auto lW = sqrt( ( piby2 - panAngle ) / piby2 * cos( panAngle ) );
        auto rW = sqrt( panAngle * sin( panAngle ) / piby2 );
        panBufferL[i] = lW;
        panBufferR[i] = rW;
    }
}

void TWSVoice::startNote (int midiNoteNumber, float velocity,
                          SynthesiserSound*, int currentPitchWheelPosition) 
{
    setPitchWheel( currentPitchWheelPosition );
    noteNum = midiNoteNumber;
    
    level = ( velocity + 64 ) / 192.0 * 0.15; // cramp up the velosity sens a bit

    nunison = std::max( 1, (int)( *(p->uni_count) ) );

    
    if( nunison == 1 )
    {
        auto cyclesPerSample = frequencyForFractionalNote( midiNoteNumber + pitchWheelNoteShift() ) / getSampleRate();
        currentAngle[0] = 0;
        angleDelta[0] = cyclesPerSample;
        pan[0] = 0;
        dDelta[0] = 0;
    }
    else
    {
        double uso = *( p->uni_spread ) / 100.0 * 2.0; // the 2.0 is for up and down
        double duso = uso / (nunison - 1);
        double sso = -uso / 2;

        level /= sqrt(nunison - 1);
        
        for( int i=0; i<nunison; ++i )
        {
            double frc = duso * i + sso;
            auto cyc = frequencyForFractionalNote( midiNoteNumber + frc + pitchWheelNoteShift() );
            auto cyclesPerSample = cyc / getSampleRate();

            currentAngle[i] = 1.0 * rand() / RAND_MAX;
            angleDelta[i] = cyclesPerSample;
            pan[i] = 2.0 * i / ( nunison - 1 ) - 1.0;
            dDelta[i] = duso * i + sso;
     ;
#if DEBUG_UNISON            
            std::cout << "UNISON i=" << i
                      << " frc=" << frc 
                      << " angleDelta[i] = " << angleDelta[i] * getSampleRate() << " pan[i] = " << pan[i] << std::endl;
#endif            
        }
    }
    priorRenderedPW = pwAmount;
    
    ampenv.setSampleRate( getSampleRate() );
    auto ap = ampenv.getParameters();
    ap.attack = *( p->amp_attack );
    ap.decay = *( p->amp_decay );
    ap.sustain = *( p->amp_sustain );
    ap.release = *( p->amp_release );
    ap.release = std::max( ap.release, 0.01f );
    ampenv.setParameters(ap);

    filtenv.setSampleRate( getSampleRate() );
    auto fp = filtenv.getParameters();
    fp.attack = *( p->filter_attack );
    fp.decay = *( p->filter_decay );
    fp.sustain = *( p->filter_sustain );
    fp.release = *( p->filter_release );
    fp.release = std::max( fp.release, 0.01f );
    filtenv.setParameters(fp);

    sinLevel.reset( 32 );
    sinLevel.setCurrentAndTargetValue( *( p->sinLevel ) );
    
    squareLevel.reset( 32 );
    squareLevel.setCurrentAndTargetValue( *( p->squareLevel ) );
    
    sawLevel.reset( 32 );
    sawLevel.setCurrentAndTargetValue( *( p->sawLevel ) );
    
    triLevel.reset( 32 );
    triLevel.setCurrentAndTargetValue( *( p->triLevel ) );

    filterCut.reset(32);
    filterCut.setCurrentAndTargetValue( *( p->filter_cutoff ) );

    filterRes.reset(32);
    filterRes.setCurrentAndTargetValue( *( p->filter_resonance ) );

    filterDepth.reset(32);
    filterDepth.setCurrentAndTargetValue( *( p->filter_depth ) );


    ampenv.noteOn();
    filtenv.noteOn();
}

void TWSVoice::stopNote (float velocity, bool allowTailOff) 
{
    ampenv.noteOff();
    filtenv.noteOff();
}

inline double analogishSquare( double x )
{
    if( x > 1 ) x-= 1;
    double r = 0;
    if( x < 0.5 )
    {
        r = 1.0 - 0.1 * ( ( x + 0.3 ) * ( x + 0.3 ) - 0.3 * 0.3 ) / ( 0.8 * 0.8 - 0.3 * 0.3 );
    }
    else
    {
        r = 0.1 * ( ( x - 0.2 ) * ( x - 0.2 ) - 0.3 * 0.3 ) / ( 0.8 * 0.8 - 0.3 * 0.3 );
    }
    return r * 2.0 - 1.0;
}

inline double analogishTri( double x )
{
    if( x > 1 ) x-= 1;
    double r = 0;
    if( x < 0.5 )
    {
        r = ( ( x + 0.3 ) * ( x + 0.3 ) - 0.3 * 0.3 ) / ( 0.8 * 0.8 - 0.3 * 0.3 );
    }
    else
    {
        r = 1.0 - ( ( x - 0.2 ) * ( x - 0.2 ) - 0.3 * 0.3 ) / ( 0.8 * 0.8 - 0.3 * 0.3 );
    }
    return r * 2.0 - 1.0;
}

inline double analogishSaw( double x )
{
    if( x > 1 ) x-= 1;
    double r = 0;
    if( x < 0.94 )
    {
        r = ( ( x + 0.3 ) * ( x + 0.3 ) - 0.3 * 0.3 ) / ( 1.4 * 1.4 - 0.3 * 0.3 );
    }
    else
    {
        auto q = x - 1;
        r = - q * q * q * q * 10000.0 + 1.0;
    }
    return r * 2.0 - 1.0;
}

void TWSVoice::renderNextBlock (AudioSampleBuffer& outputBuffer, int startSample, int numSamples) 
{
    if( ! isVoiceActive() ) return;
    const int resetFilterEvery = 4;

    sinLevel.setTargetValue( *( p->sinLevel ) );
    squareLevel.setTargetValue( *( p->squareLevel ) );
    sawLevel.setTargetValue( *( p->sawLevel ) );
    triLevel.setTargetValue( *( p->triLevel ) );

    filterCut.setTargetValue( *( p->filter_cutoff ) );
    filterRes.setTargetValue( *( p->filter_resonance ) );
    filterDepth.setTargetValue( *( p->filter_depth ) );

    bool recalcCycle = false;
    if( priorRenderedPW != pwAmount )
    {
        priorRenderedPW = pwAmount;
        recalcCycle = true;
    }

    int sc = 0;
    while (--numSamples >= 0)
    {
        // doto - lose the current
        auto sampleL = 0.0f;
        auto sampleR = 0.0f;

        auto AEG = ampenv.getNextSample() * level;
        auto FEG = filtenv.getNextSample();
        
        auto sinlv = sinLevel.getNextValue();
        auto sqrlv = squareLevel.getNextValue();
        auto sawlv = sawLevel.getNextValue();
        auto trilv = triLevel.getNextValue();
        
        auto filtd = filterDepth.getNextValue();
        auto filtc = std::max( 10.0, std::min( filterCut.getNextValue() * ( 1 + FEG * FEG * filtd * 64.0 ), getSampleRate() / 2 ) );
        auto filtr = filterRes.getNextValue();

        // This is overkill but for now just hammer it
        if( sc % resetFilterEvery == 0 )
        {
            auto c = IIRCoefficients::makeLowPass( getSampleRate(), filtc, filtr );
            filterL.setCoefficients( c );
            filterR.setCoefficients( c );
        }
        sc++;
        
        for( int i=0; i<nunison; ++i )
        {
            auto ca = currentAngle[i];

            // Obviously these waveforms suck
            auto oscSin = (float) std::sin ( 2.0 * MathConstants<double>::pi * ca) * sinlv;
            auto oscSqr = analogishSquare(ca + 0.05) * sqrlv;
            auto oscSaw = analogishSaw(ca + 0.07) * sawlv;
            auto oscTri = analogishTri(ca + 0.03) * trilv;

            auto currentVoice = AEG * ( oscSqr + oscSin + oscSaw + oscTri );

            int panIdx = ( pan[i] + 1 ) * N_PAN / 2;
            if( panIdx < 0 ) panIdx = 0;
            if( panIdx >= N_PAN ) panIdx = N_PAN - 1;

            // This isn't precise enough to bother interpolating
            auto lW = panBufferL[panIdx];
            auto rW = panBufferR[panIdx];

            // std::cout << "lW/rW=" << lW << " " << rW << " " << panAngle << std::endl;
            sampleL += lW * currentVoice;
            sampleR += rW * currentVoice;

            if( recalcCycle )
            {
                auto cyc = frequencyForFractionalNote( noteNum + dDelta[i] + pitchWheelNoteShift() );
                auto cyclesPerSample = cyc / getSampleRate();
                angleDelta[i] = cyclesPerSample;
            }
            
            currentAngle[i] += angleDelta[i];
            if( currentAngle[i] > 1.0 )
                currentAngle[i] -= 1.0;
        }

        sampleL = filterL.processSingleSampleRaw( sampleL );
        sampleR = filterL.processSingleSampleRaw( sampleR );
        
        if( outputBuffer.getNumChannels() == 1 )
        {
            outputBuffer.addSample (0, startSample, ( sampleL + sampleR ) / 2.0 );
        }
        else 
        {
            outputBuffer.addSample (0, startSample, sampleL);
            outputBuffer.addSample (1, startSample, sampleR);
        }
        
        ++startSample;
    }
    
    // TODO: If the AEG is over kill the note
    if( ! ampenv.isActive() && isVoiceActive() )
    {
        clearCurrentNote();
    }
    

}


double TWSVoice::frequencyForFractionalNote( double fnote )
{
    /*
     * TODO: We know frequency is actually exponential in note so should we interp differently?
     * If so - push that answer to the library not here.
     */
    int bn = std::floor( fnote );
    double frac = fnote - bn;

    auto noteDown = p->tuning.frequencyForMidiNote( bn );
    auto noteUp   = p->tuning.frequencyForMidiNote( bn + 1 );

    auto res = noteDown * ( 1 - frac ) + noteUp * frac;
    
    return res;
}

double TWSVoice::pitchWheelNoteShift() {
    if( pwAmount == 0 )
        return 0;
    else if( pwAmount < 0 )
        return pwAmount * ( *(p->pb_down ));
    else
        return pwAmount * ( *(p->pb_up ));
}
