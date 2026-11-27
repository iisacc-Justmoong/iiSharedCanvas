#pragma once

#include "Timeline/FrameMatching.h"
#include <boost/multiprecision/cpp_int.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <stdexcept>
#include <vector>

// Exact algebraic time mapping. Every finite double is converted to its exact
// binary rational; all root isolation, comparisons and branch decisions use
// integer/rational arithmetic. Values and scratch storage are call-local.
namespace iiSharedCanvas::exact_timeline {
using Integer=boost::multiprecision::cpp_int;
using Rational=boost::multiprecision::cpp_rational;
using Polynomial=std::vector<Rational>; // Coefficients in ascending order.

inline Rational fromDouble(double value) {
    const auto bits=std::bit_cast<std::uint64_t>(value);
    const auto exponent=unsigned((bits>>52)&0x7ffU);
    const auto fraction=bits&((std::uint64_t{1}<<52)-1);
    if(exponent==0x7ffU)throw std::invalid_argument("non-finite Bezier tangent");
    Integer magnitude=exponent ? (fraction|(std::uint64_t{1}<<52)) : fraction;
    const int power=exponent ? int(exponent)-1075 : -1074;
    if((bits>>63)!=0)magnitude=-magnitude;
    // Construct the rational from its two components so zero and powers of
    // two are reduced before any polynomial operation. In particular, an
    // unreduced zero denominator can make exact cancellation fail to trim a
    // polynomial and cause Euclidean division not to terminate.
    if(magnitude==0)return Rational(0);
    if(power>=0)return Rational(magnitude<<power);
    return Rational(magnitude,Integer(1)<<(-power));
}
inline Rational fromFraction(std::int64_t n,std::int64_t d) {
    if(d<=0)throw std::invalid_argument("invalid rational denominator");
    return Rational(n,d);
}
inline TimelineExactRational publicRational(const Rational &value) {
    const Rational normalized(numerator(value),denominator(value));
    return {numerator(normalized).convert_to<std::string>(),denominator(normalized).convert_to<std::string>()};
}
struct Bezier {
    std::array<Rational,4> time,value;
    Rational target;
};
inline Bezier makeBezier(const TimelineTimeMapPoint &a,const TimelineTimeMapPoint &b,const Rational &target) {
    return {{{Rational(a.timelineTicks),Rational(a.timelineTicks)+fromDouble(a.outgoing.timeOffset),
              Rational(b.timelineTicks)+fromDouble(b.incoming.timeOffset),Rational(b.timelineTicks)}},
            {{Rational(a.sourceTicks),Rational(a.sourceTicks)+fromDouble(a.outgoing.valueOffset),
              Rational(b.sourceTicks)+fromDouble(b.incoming.valueOffset),Rational(b.sourceTicks)}},target};
}
// A physical timeline has a nondecreasing time coordinate. The derivative is
// quadratic; testing its endpoints and its exact interior minimum is complete.
inline bool monotoneTime(const TimelineTimeMapPoint &a,const TimelineTimeMapPoint &b) {
    const auto curve=makeBezier(a,b,0);
    if(curve.time[3]<=curve.time[0])return false;
    const Rational d0=curve.time[1]-curve.time[0],d1=curve.time[2]-curve.time[1],d2=curve.time[3]-curve.time[2];
    if(d0<0 || d2<0)return false;
    const Rational aa=d0-2*d1+d2, bb=2*(d1-d0);
    if(aa>0){const Rational vertex=-bb/(2*aa);if(vertex>0 && vertex<1 && aa*vertex*vertex+bb*vertex+d0<0)return false;}
    return true;
}
inline Polynomial polynomial(const std::array<Rational,4> &v) {
    return {v[0],3*(v[1]-v[0]),3*(v[0]-2*v[1]+v[2]),-v[0]+3*v[1]-3*v[2]+v[3]};
}
inline void trim(Polynomial &p){while(!p.empty() && p.back()==0)p.pop_back();}
inline Polynomial derivative(const Polynomial &p){Polynomial d;if(p.size()>1){d.reserve(p.size()-1);for(size_t i=1;i<p.size();++i)d.push_back(p[i]*i);}trim(d);return d;}
inline Rational evaluate(const Polynomial &p,const Rational &x){Rational result=0;for(auto i=p.rbegin();i!=p.rend();++i)result=result*x+*i;return result;}
inline Polynomial remainder(Polynomial a,const Polynomial &b) {
    if(b.empty())throw std::logic_error("polynomial division by zero");
    trim(a);while(a.size()>=b.size() && !a.empty()){
        const auto offset=a.size()-b.size();const Rational factor=a.back()/b.back();
        for(size_t i=0;i<b.size();++i)a[offset+i]-=factor*b[i];trim(a);
    }return a;
}
inline Polynomial gcd(Polynomial a,Polynomial b){trim(a);trim(b);while(!b.empty()){auto r=remainder(a,b);a=std::move(b);b=std::move(r);}if(!a.empty()){const auto lead=a.back();for(auto &v:a)v/=lead;}return a;}
inline int sign(const Rational &v){return v<0?-1:v>0?1:0;}
inline int variations(const std::vector<Polynomial> &sequence,const Rational &x){int count=0,previous=0;for(const auto &p:sequence){const int next=sign(evaluate(p,x));if(!next)continue;if(previous && next!=previous)++count;previous=next;}return count;}
inline bool hasRootInUnit(const Polynomial &input) {
    Polynomial first=input;trim(first);if(first.size()<2)return false;
    std::vector<Polynomial> sturm{first,derivative(first)};
    while(!sturm.back().empty()){
        auto next=remainder(sturm[sturm.size()-2],sturm.back());
        for(auto &v:next)v=-v;trim(next);if(next.empty())break;sturm.push_back(std::move(next));
    }
    return variations(sturm,0)>variations(sturm,1);
}
struct Interval { Rational low,high; };
inline Interval multiply(Interval a,Interval b){
    std::array<Rational,4> values{a.low*b.low,a.low*b.high,a.high*b.low,a.high*b.high};
    const auto [low,high]=std::minmax_element(values.begin(),values.end());return {*low,*high};
}
inline Interval bound(const Polynomial &p,const Rational &lo,const Rational &hi){
    Interval result{0,0};for(auto i=p.rbegin();i!=p.rend();++i){result=multiply(result,{lo,hi});result.low+=*i;result.high+=*i;}return result;
}
// The time curve is monotone with distinct endpoints, hence the root is
// unique. Polynomial GCD and Sturm counting detect exact equality; otherwise
// rational interval refinement terminates when the sign is proven.
inline int signAtRoot(const Bezier &curve,Polynomial query) {
    trim(query);if(query.empty())return 0;
    auto equation=polynomial(curve.time);equation[0]-=curve.target;trim(equation);
    if(evaluate(equation,0)==0)return sign(evaluate(query,0));
    if(evaluate(equation,1)==0)return sign(evaluate(query,1));
    if(hasRootInUnit(gcd(equation,query)))return 0;
    Rational low=0,high=1;
    for(;;){
        const auto interval=bound(query,low,high);
        if(interval.low>0)return 1;if(interval.high<0)return -1;
        const Rational mid=(low+high)/2;const int timeSign=sign(evaluate(equation,mid));
        if(!timeSign)return sign(evaluate(query,mid));
        if(timeSign<0)low=mid;else high=mid;
    }
}
inline int compare(const Bezier &curve,const Rational &value){auto q=polynomial(curve.value);q[0]-=value;return signAtRoot(curve,std::move(q));}
inline bool leftLimit(const Bezier &curve,bool incomingLeft){
    auto q=polynomial(curve.value);
    for(unsigned order=1;order<=3;++order){q=derivative(q);const int s=signAtRoot(curve,q);if(s)return (incomingLeft && (order&1U)) ? s>0 : s<0;}
    return false;
}
inline TimelineBezierSourcePosition publicExpression(const Bezier &curve){
    TimelineBezierSourcePosition result;
    for(size_t i=0;i<4;++i){result.timeControl[i]=publicRational(curve.time[i]);result.valueControl[i]=publicRational(curve.value[i]);}
    result.target=publicRational(curve.target);return result;
}
} // namespace iiSharedCanvas::exact_timeline
